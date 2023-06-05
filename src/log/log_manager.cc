
#include <cassert>
#include <iostream>
#include <string.h>
#include <cstddef>
#include <set>
#include "log/log_manager.h"
#include "common/macros.h"
#include "storage/test_file.h"

namespace buzzdb {

/**
 * Functionality of the buffer manager that might be handy

 Flush all the dirty pages to the disk 
 	buffer_manager.flush_all_pages(): 

 Write @data of @length at an @offset the buffer page @page_id 
 	BufferFrame& frame = buffer_manager.fix_page(page_id, true);
	memcpy(&frame.get_data()[offset], data, length);
	buffer_manager.unfix_page(frame, true);

 * Read and Write from/to the log_file
   log_file_->read_block(offset, size, data);
   
   Usage:
   uint64_t txn_id;
   log_file_->read_block(offset, sizeof(uint64_t), reinterpret_cast<char *>(&txn_id));
   log_file_->write_block(reinterpret_cast<char *> (&txn_id), offset, sizeof(uint64_t));
 */


LogManager::LogManager(File* log_file){
	log_file_ = log_file;
	log_record_type_to_count[LogRecordType::ABORT_RECORD] = 0;
	log_record_type_to_count[LogRecordType::COMMIT_RECORD] = 0;
	log_record_type_to_count[LogRecordType::UPDATE_RECORD] = 0;
	log_record_type_to_count[LogRecordType::BEGIN_RECORD] = 0;
	log_record_type_to_count[LogRecordType::CHECKPOINT_RECORD] = 0;
}

LogManager::~LogManager(){
}

void LogManager::reset(File* log_file){
	log_file_ = log_file;
	current_offset_ = 0;
	txn_id_to_first_log_record.clear();
	log_record_type_to_count.clear();
}


/// Get log records
uint64_t LogManager::get_total_log_records(){
	uint64_t record_count = log_record_type_to_count[LogRecordType::ABORT_RECORD] + log_record_type_to_count[LogRecordType::COMMIT_RECORD] + log_record_type_to_count[LogRecordType::UPDATE_RECORD] + log_record_type_to_count[LogRecordType::BEGIN_RECORD] + log_record_type_to_count[LogRecordType::CHECKPOINT_RECORD];
	return record_count;
}

uint64_t LogManager::get_total_log_records_of_type(UNUSED_ATTRIBUTE LogRecordType type){
	uint64_t record_count_type = log_record_type_to_count[type];
	return record_count_type;
}

/** 
 * Increment the ABORT_RECORD count.
 * Rollback the provided transaction.
 * Add abort log record to the log file.
 * Remove from the active transactions.
*/
void LogManager::log_abort(UNUSED_ATTRIBUTE uint64_t txn_id, UNUSED_ATTRIBUTE BufferManager& buffer_manager){
	std::map<LogRecordType, uint64_t>::iterator it = log_record_type_to_count.find(LogRecordType::ABORT_RECORD);
	if (it != log_record_type_to_count.end())
    	it->second += 1;

	LogRecord record;
	memset(&record, 0, sizeof(LogRecord));

	record.log_type = LogRecordType::ABORT_RECORD;
	record.txn_id = txn_id;

	log_file_->write_block(reinterpret_cast<char *> (&record), current_offset_, sizeof(LogRecord));
	current_offset_ += sizeof(LogRecord);

	uint64_t LSN = get_total_log_records();
	uint64_t lastLSN = active_txn_table[txn_id];

	prevLSN.insert({LSN, lastLSN});

	rollback_txn(txn_id, buffer_manager);

	active_txn_table.erase(txn_id);

	buffer_manager.flush_all_pages();

	return;	
}

/**
 * Increment the COMMIT_RECORD count
 * Add commit log record to the log file
 * Remove from the active transactions
*/ 
void LogManager::log_commit(UNUSED_ATTRIBUTE uint64_t txn_id){
	std::map<LogRecordType, uint64_t>::iterator it = log_record_type_to_count.find(LogRecordType::COMMIT_RECORD);
	if (it != log_record_type_to_count.end())
    	it->second += 1;
	
	LogRecord record;
	memset(&record, 0, sizeof(LogRecord));

	record.log_type = LogRecordType::COMMIT_RECORD;
	record.txn_id = txn_id;

	log_file_->write_block(reinterpret_cast<char *> (&record), current_offset_, sizeof(LogRecord));
	current_offset_ += sizeof(LogRecord);

	active_txn_table.erase(txn_id);

}

/**
 * Increment the UPDATE_RECORD count
 * Add the update log record to the log file
 * @param txn_id		transaction id
 * @param page_id		buffer page id
 * @param length		length of the update tuple
 * @param offset 		offset to the tuple in the buffer page
 * @param before_img	before image of the buffer page at the given offset 
 * @param after_img		after image of the buffer page at the given offset	
 */
void LogManager::log_update(UNUSED_ATTRIBUTE uint64_t txn_id, UNUSED_ATTRIBUTE uint64_t page_id, UNUSED_ATTRIBUTE uint64_t length,
							UNUSED_ATTRIBUTE uint64_t offset, UNUSED_ATTRIBUTE std::byte* before_img, UNUSED_ATTRIBUTE std::byte* after_img){
	std::map<LogRecordType, uint64_t>::iterator it = log_record_type_to_count.find(LogRecordType::UPDATE_RECORD);
	if (it != log_record_type_to_count.end())
    	it->second += 1;

	LogRecord record;
	memset(&record, 0, sizeof(LogRecord));

	record.log_type = LogRecordType::UPDATE_RECORD;
	record.txn_id = txn_id;
	record.page_id = page_id;
	record.length = length;
	record.offset = offset;
	record.before_img = before_img;
	record.after_img = after_img;

	log_file_->write_block(reinterpret_cast<char *> (&record), current_offset_, sizeof(LogRecord));

	current_offset_ += sizeof(LogRecord);

	uint64_t LSN = get_total_log_records();
	uint64_t lastLSN = active_txn_table[txn_id];

	prevLSN.insert({LSN, lastLSN});

	std::map<uint64_t, uint64_t>::iterator iterate = active_txn_table.find(txn_id);
	if (iterate != active_txn_table.end())
    	iterate->second = LSN;

}

/**
 * Increment the BEGIN_RECORD count
 * Add the begin log record to the log file
 * Add to the active transactions
 */ 
void LogManager::log_txn_begin(UNUSED_ATTRIBUTE uint64_t txn_id){
	std::map<LogRecordType, uint64_t>::iterator it = log_record_type_to_count.find(LogRecordType::BEGIN_RECORD);
	if (it != log_record_type_to_count.end())
    	it->second += 1;

	LogRecord record;
	memset(&record, 0, sizeof(LogRecord));

	record.log_type = LogRecordType::BEGIN_RECORD;
	record.txn_id = txn_id;
	
	log_file_->write_block(reinterpret_cast<char *> (&record), current_offset_, sizeof(LogRecord));
	current_offset_ += sizeof(LogRecord);

	uint64_t LSN = get_total_log_records();
	active_txn_table.insert({txn_id, LSN});

	txn_id_to_first_log_record.insert({txn_id, LSN});

	prevLSN.insert({LSN, nill});

}



/**
 * Increment the CHECKPOINT_RECORD count
 * Flush all dirty pages to the disk (USE: buffer_manager.flush_all_pages())
 * Add the checkpoint log record to the log file
 */ 
void LogManager::log_checkpoint(UNUSED_ATTRIBUTE BufferManager& buffer_manager){
	std::map<LogRecordType, uint64_t>::iterator it = log_record_type_to_count.find(LogRecordType::CHECKPOINT_RECORD);
	if (it != log_record_type_to_count.end())
    	it->second += 1;	

	LogRecord record;
	memset(&record, 0, sizeof(LogRecord));

	record.log_type = LogRecordType::CHECKPOINT_RECORD;
	
	log_file_->write_block(reinterpret_cast<char *> (&record), current_offset_, sizeof(LogRecord));
	current_offset_ += sizeof(LogRecord);

	buffer_manager.flush_all_pages();
}

/**
 * @Analysis Phase: 
 * 		1. Get the active transactions and commited transactions 
 * 		2. Restore the txn_id_to_first_log_record
 * @Redo Phase:
 * 		1. Redo the entire log tape to restore the buffer page
 * 		2. For UPDATE logs: write the after_img to the buffer page
 * 		3. For ABORT logs: rollback the transactions
 * 	@Undo Phase
 * 		1. Rollback the transactions which are active and not commited
 */ 
void LogManager::recovery(UNUSED_ATTRIBUTE BufferManager& buffer_manager){
	
	std::map<uint64_t, uint64_t>::iterator it;

	for (it = active_txn_table.begin(); it != active_txn_table.end(); it++){
		uint64_t txn_id = it->first;
		rollback_txn(txn_id, buffer_manager);	
	}

	active_txn_table.clear();

	buffer_manager.flush_all_pages();

}


/**
 * Use txn_id_to_first_log_record to get the begin of the current transaction
 * Walk through the log tape and rollback the changes by writing the before
 * image of the tuple on the buffer page.
 * Note: There might be other transactions' log records interleaved, so be careful to
 * only undo the changes corresponding to current transactions.  
 */ 
void LogManager::rollback_txn(UNUSED_ATTRIBUTE uint64_t txn_id, UNUSED_ATTRIBUTE BufferManager& buffer_manager){

	uint64_t lastLSN = active_txn_table[txn_id];

	uint64_t prev = lastLSN;

	while (prev != nill){
		
		size_t ofst = (prev - 1) * sizeof(LogRecord);
		size_t size = sizeof(LogRecord);
		auto p = std::make_unique<char[]>(size);
		log_file_->read_block(ofst, size, p.get());

		auto* prevRecord = reinterpret_cast<LogRecord*>(p.get());

		if (prevRecord->log_type == LogRecordType::UPDATE_RECORD){

			std::byte* before_img = prevRecord->before_img;
			uint64_t page_id = prevRecord->page_id;
			uint64_t offset = prevRecord->offset;
			uint64_t record_size = prevRecord->length;

			std::vector<char> before_record;
			before_record.resize(record_size);
			memcpy(before_record.data(), &before_img, record_size);

			BufferFrame& frame = buffer_manager.fix_page(page_id, true);

			memcpy(&frame.get_data()[offset], reinterpret_cast<std::byte *> (before_record.data()), record_size);

			buffer_manager.unfix_page(frame, true);
		}

		prev = prevLSN[prev];

	}

}

}  // namespace buzzdb
