/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Longda on 2021/4/13.
//
#include "storage/common/record_manager.h"
#include "rc.h"
#include "common/log/log.h"
#include "common/lang/bitmap.h"
#include "condition_filter.h"

using namespace common;

struct PageHeader
{
  int record_num;          // 当前页面记录的个数
  int record_capacity;     // 最大记录个数
  int record_real_size;    // 每条记录的实际大小
  int record_size;         // 每条记录占用实际空间大小(可能对齐)
  int first_record_offset; // 第一条记录的偏移量
  int has_next;           // 是否有下一页
  PageNum next_page_num;
};

int align8(int size)
{ // 用于 size 对齐
  return size / 8 * 8 + ((size % 8 == 0) ? 0 : 8);
}

int page_fix_size()
{
    // 都是int
  return sizeof(PageHeader::record_num) + sizeof(PageHeader::record_capacity) + sizeof(PageHeader::record_real_size) + 
         sizeof(PageHeader::record_size) + sizeof(PageHeader::first_record_offset) + sizeof(PageHeader::has_next) +
         sizeof(PageHeader::next_page_num);
}

int page_record_capacity(int page_size, int record_size)
{
  // (record_capacity * record_size) + record_capacity/8 + 1 <= (page_size - fix_size)
  // ==> record_capacity = ((page_size - fix_size) - 1) / (record_size + 0.125)
  return (int)((page_size - page_fix_size() - 1) / (record_size + 0.125));
}

int page_bitmap_size(int record_capacity)
{
  return record_capacity / 8 + ((record_capacity % 8 == 0) ? 0 : 1);
}

int page_header_size(int record_capacity)
{
  const int bitmap_size = page_bitmap_size(record_capacity);
  // LOG_INFO("bitmap_size: %d!!!!!!!!!!!!!!!!!!", bitmap_size);
  return align8(page_fix_size() + bitmap_size);
}
////////////////////////////////////////////////////////////////////////////////
RecordPageHandler::RecordPageHandler() : disk_buffer_pool_(nullptr),
                                         file_id_(-1),
                                         page_header_(nullptr),
                                         bitmap_(nullptr)
{
  page_handle_.open = false;
  page_handle_.frame = nullptr;
}

RecordPageHandler::~RecordPageHandler()
{
  deinit();
}

RC RecordPageHandler::init(DiskBufferPool &buffer_pool, int file_id, PageNum page_num)
{
  if (disk_buffer_pool_ != nullptr)
  {
    LOG_WARN("Disk buffer pool has been opened for file_id:page_num %d:%d.",
             file_id, page_num);
    return RC::RECORD_OPENNED;
  }

  RC ret = RC::SUCCESS;
  if ((ret = buffer_pool.get_this_page(file_id, page_num, &page_handle_)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to get page handle from disk buffer pool. ret=%d:%s", ret, strrc(ret));
    return ret;
  }
  // 1. 这里的data虽然会在函数作用域结束后消失，但是已经在内存上某个地址放置了data具体数据
  char *data;
  ret = buffer_pool.get_data(&page_handle_, &data);
  if (ret != RC::SUCCESS)
  {
    LOG_ERROR("Failed to get page data. ret=%d:%s", ret, strrc(ret));
    return ret;
  }

  disk_buffer_pool_ = &buffer_pool;
  file_id_ = file_id;
  // 2. 后面data指针会被销毁，但是这里已经地址传给了当前类的中指针，存放具体数据的地址已经留存下来了
  page_header_ = (PageHeader *)(data);
  bitmap_ = data + page_fix_size();
  LOG_TRACE("Successfully init file_id:page_num %d:%d.", file_id, page_num);
  return ret;
}

RC RecordPageHandler::init_empty_page(DiskBufferPool &buffer_pool, int file_id, PageNum page_num, int record_size)
{
  RC ret = init(buffer_pool, file_id, page_num);
  if (ret != RC::SUCCESS)
  {
    LOG_ERROR("Failed to init empty page file_id:page_num:record_size %d:%d:%d.", file_id, page_num, record_size);
    return ret;
  }

  int page_size = sizeof(page_handle_.frame->page.data);
  int record_phy_size = align8(record_size);
  page_header_->has_next = 0;
  page_header_->next_page_num = -1;
  page_header_->record_num = 0;
  page_header_->record_capacity = page_record_capacity(page_size, record_phy_size);
  page_header_->record_real_size = record_size;
  page_header_->record_size = record_phy_size;
  page_header_->first_record_offset = page_header_size(page_header_->record_capacity);
  bitmap_ = page_handle_.frame->page.data + page_fix_size();

  memset(bitmap_, 0, page_bitmap_size(page_header_->record_capacity));
  ret = disk_buffer_pool_->mark_dirty(&page_handle_);
  if (ret != RC::SUCCESS)
  {
    LOG_ERROR("Failed to mark page dirty. ret=%s", strrc(ret));
  }

  return RC::SUCCESS;
}

RC RecordPageHandler::deinit()
{
  if (disk_buffer_pool_ != nullptr)
  {
    RC rc = disk_buffer_pool_->unpin_page(&page_handle_);
    if (rc != RC::SUCCESS)
    {
      LOG_ERROR("Failed to unpin page when deinit record page handler. rc=%s", strrc(rc));
    }
    disk_buffer_pool_ = nullptr;
  }

  return RC::SUCCESS;
}

RC RecordPageHandler::insert_record(const char *data, RID *rid)
{

  if (page_header_->record_num == page_header_->record_capacity)
  {
    LOG_WARN("Page is full, file_id:page_num %d:%d.", file_id_,
             page_handle_.frame->page.page_num);
    return RC::RECORD_NOMEM;
  }
    
  // 找到空闲位置
  Bitmap bitmap(bitmap_, page_header_->record_capacity);
  int index = bitmap.next_unsetted_bit(0);
  bitmap.set_bit(index);
  page_header_->record_num++;

  // assert index < page_header_->record_capacity
  char *record_data = page_handle_.frame->page.data +
                      page_header_->first_record_offset + (index * page_header_->record_size);
  memcpy(record_data, data, page_header_->record_real_size);

  RC rc = disk_buffer_pool_->mark_dirty(&page_handle_);
  if (rc != RC::SUCCESS)
  {
    LOG_ERROR("Failed to mark page dirty. rc =%d:%s", rc, strrc(rc));
    // hard to rollback
  }

  if (rid)
  {
    rid->page_num = get_page_num();
    rid->slot_num = index;
  }

  LOG_TRACE("Insert record. rid page_num=%d, slot num=%d", get_page_num(), index);
  return RC::SUCCESS;
}

RC RecordPageHandler::update_record(const Record *rec)
{
  RC ret = RC::SUCCESS;

  if (rec->rid.slot_num >= page_header_->record_capacity)
  {
    LOG_ERROR("Invalid slot_num %d, exceed page's record capacity, file_id:page_num %d:%d.",
              rec->rid.slot_num,
              file_id_,
              page_handle_.frame->page.page_num);
    return RC::INVALID_ARGUMENT;
  }

  Bitmap bitmap(bitmap_, page_header_->record_capacity);
  if (!bitmap.get_bit(rec->rid.slot_num))
  {
    LOG_ERROR("Invalid slot_num %d, slot is empty, file_id:page_num %d:%d.",
              rec->rid.slot_num,
              file_id_,
              page_handle_.frame->page.page_num);
    ret = RC::RECORD_RECORD_NOT_EXIST;
  }
  else
  {
    char *record_data = page_handle_.frame->page.data +
                        page_header_->first_record_offset + (rec->rid.slot_num * page_header_->record_size);
    memcpy(record_data, rec->data, page_header_->record_real_size);
    ret = disk_buffer_pool_->mark_dirty(&page_handle_);
    if (ret != RC::SUCCESS)
    {
      LOG_ERROR("Failed to mark page dirty. ret=%s", strrc(ret));
    }
  }

  LOG_TRACE("Update record. page num=%d,slot=%d", rec->rid.page_num, rec->rid.slot_num);
  return ret;
}

RC RecordPageHandler::delete_record(const RID *rid)
{
  RC ret = RC::SUCCESS;

  if (rid->slot_num >= page_header_->record_capacity)
  {
    LOG_ERROR("Invalid slot_num %d, exceed page's record capacity, file_id:page_num %d:%d.",
              rid->slot_num,
              file_id_,
              page_handle_.frame->page.page_num);
    return RC::INVALID_ARGUMENT;
  }

  Bitmap bitmap(bitmap_, page_header_->record_capacity);
  if (bitmap.get_bit(rid->slot_num))
  {
    bitmap.clear_bit(rid->slot_num);
    page_header_->record_num--;
    page_header_->has_next = 0;
    page_header_->next_page_num = -1;
    ret = disk_buffer_pool_->mark_dirty(&page_handle_);
    if (ret != RC::SUCCESS)
    {
      LOG_ERROR("failed to mark page dirty in delete record. ret=%d:%s", ret, strrc(ret));
      // hard to rollback
    }

    if (page_header_->record_num == 0)
    {
      DiskBufferPool *disk_buffer_pool = disk_buffer_pool_;
      int file_id = file_id_;
      PageNum page_num = get_page_num();
      deinit();
      disk_buffer_pool->dispose_page(file_id, page_num);
    }
  }
  else
  {
    LOG_ERROR("Invalid slot_num %d, slot is empty, file_id:page_num %d:%d.",
              rid->slot_num,
              file_id_,
              page_handle_.frame->page.page_num);
    RC::RECORD_RECORD_NOT_EXIST;
  }
  return ret;
}

RC RecordPageHandler::get_record(const RID *rid, Record *rec)
{
  if (rid->slot_num >= page_header_->record_capacity)
  {
    LOG_ERROR("Invalid slot_num:%d, exceed page's record capacity, file_id:page_num %d:%d.",
              rid->slot_num,
              file_id_,
              page_handle_.frame->page.page_num);
    return RC::RECORD_INVALIDRID;
  }

  Bitmap bitmap(bitmap_, page_header_->record_capacity);
  if (!bitmap.get_bit(rid->slot_num))
  {
    LOG_ERROR("Invalid slot_num:%d, slot is empty, file_id:page_num %d:%d.",
              rid->slot_num,
              file_id_,
              page_handle_.frame->page.page_num);
    return RC::RECORD_RECORD_NOT_EXIST;
  }

  char *data = page_handle_.frame->page.data +
               page_header_->first_record_offset + (page_header_->record_size * rid->slot_num);

  // rec->valid = true;
  rec->rid = *rid;
  rec->data = data;
  return RC::SUCCESS;
}

RC RecordPageHandler::get_first_record(Record *rec)
{
  rec->rid.slot_num = -1;
  return get_next_record(rec);
}

RC RecordPageHandler::get_next_record(Record *rec)
{
  if (rec->rid.slot_num >= page_header_->record_capacity - 1)
  {
    LOG_ERROR("Invalid slot_num:%d, exceed page's record capacity: %d, file_id:page_num %d:%d.",
              rec->rid.slot_num,
              page_header_->record_capacity,
              file_id_,
              page_handle_.frame->page.page_num);
    return RC::RECORD_EOF;
  }

  Bitmap bitmap(bitmap_, page_header_->record_capacity);
  int index = bitmap.next_setted_bit(rec->rid.slot_num + 1);

  if (index < 0)
  {
    LOG_TRACE("There is no empty slot, file_id:page_num %d:%d.",
              file_id_,
              page_handle_.frame->page.page_num);
    return RC::RECORD_EOF;
  }

  rec->rid.page_num = get_page_num();
  rec->rid.slot_num = index;
  // rec->valid = true;

  // 这里拿到的数据已经加了偏移
  char *record_data = page_handle_.frame->page.data +
                      page_header_->first_record_offset + (index * page_header_->record_size);
  rec->data = record_data;
  return RC::SUCCESS;
}

PageNum RecordPageHandler::get_page_num() const
{
  if (nullptr == page_header_)
  {
    return (PageNum)(-1);
  }
  return page_handle_.frame->page.page_num;
}

bool RecordPageHandler::is_full() const
{
  return page_header_->record_num >= page_header_->record_capacity;
}

////////////////////////////////////////////////////////////////////////////////

RecordFileHandler::RecordFileHandler() : disk_buffer_pool_(nullptr),
                                         file_id_(-1)
{
}

RC RecordFileHandler::init(DiskBufferPool &buffer_pool, int file_id)
{

  RC ret = RC::SUCCESS;

  if (disk_buffer_pool_ != nullptr)
  {
    LOG_ERROR("%d has been openned.", file_id);
    return RC::RECORD_OPENNED;
  }

  disk_buffer_pool_ = &buffer_pool;
  file_id_ = file_id;

  LOG_TRACE("Successfully open %d.", file_id);
  return ret;
}

void RecordFileHandler::close()
{
  if (disk_buffer_pool_ != nullptr)
  {
    disk_buffer_pool_ = nullptr;
  }
}

RC RecordFileHandler::insert_record(const char *data, int record_size, RID *rid)
{
    if (record_size > 4096) {
        return insert_record_with_text(data, record_size, rid);
    }

  RC ret = RC::SUCCESS;
  // 找到没有填满的页面
  int page_count = 0;
  if ((ret = disk_buffer_pool_->get_page_count(file_id_, &page_count)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to get page count while inserting record");
    return ret;
  }

  PageNum current_page_num = record_page_handler_.get_page_num();
  if (current_page_num < 0)
  {
    if (page_count >= 2)
    { // 当前buffer pool 有页面时才尝试加载第一页
      // 参考diskBufferPool，pageNum从1开始
      if ((ret = record_page_handler_.init(*disk_buffer_pool_, file_id_, 1)) != RC::SUCCESS)
      {
        LOG_ERROR("Failed to init record page handler.ret=%d", ret);
        return ret;
      }
      current_page_num = record_page_handler_.get_page_num();
    }
    else
    {
      current_page_num = 0;
    }
  }

  bool page_found = false;
  for (int i = 0; i < page_count; i++)
  {
    current_page_num = (current_page_num + i) % page_count; // 从当前打开的页面开始查找
    if (current_page_num == 0)
    {
      continue;
    }
    if (current_page_num != record_page_handler_.get_page_num())
    {
      record_page_handler_.deinit();
      ret = record_page_handler_.init(*disk_buffer_pool_, file_id_, current_page_num);
      if (ret != RC::SUCCESS && ret != RC::BUFFERPOOL_INVALID_PAGE_NUM)
      {
        LOG_ERROR("Failed to init record page handler. page number is %d. ret=%d:%s", current_page_num, ret, strrc(ret));
        return ret;
      }
    }

    if (!record_page_handler_.is_full())
    {
      page_found = true;
      break;
    }
  }

  // 找不到就分配一个新的页面
  if (!page_found)
  {
    BPPageHandle page_handle;
    
    if ((ret = disk_buffer_pool_->allocate_page(file_id_, &page_handle)) != RC::SUCCESS)
    {
      LOG_ERROR("Failed to allocate page while inserting record. file_it:%d, ret:%d",
                file_id_, ret);
      return ret;
    }
    LOG_INFO("!!!!!!!!!!!!!!!!!BEFORE page %d's pin count: %d !!!!!!!!!!!!!!!!", page_handle.frame->page.page_num, page_handle.frame->pin_count);
    current_page_num = page_handle.frame->page.page_num;
    record_page_handler_.deinit();
    
    ret = record_page_handler_.init_empty_page(*disk_buffer_pool_, file_id_, current_page_num, record_size);
    LOG_INFO("!!!!!!!!!!!!!!!!!BEFORE page %d's pin count: %d !!!!!!!!!!!!!!!!", page_handle.frame->page.page_num, page_handle.frame->pin_count);
    if (ret != RC::SUCCESS)
    {
      LOG_ERROR("Failed to init empty page. file_id:%d, ret:%d", file_id_, ret);
      if (RC::SUCCESS != disk_buffer_pool_->unpin_page(&page_handle))
      {
        LOG_ERROR("Failed to unpin page. file_id:%d", file_id_);
      }
      return ret;
    }
    if (RC::SUCCESS != disk_buffer_pool_->unpin_page(&page_handle))
    {
      LOG_ERROR("Failed to unpin page. file_id:%d", file_id_);
    }
    LOG_INFO("!!!!!!!!!!!!!!!!!BEFORE page %d's pin count: %d !!!!!!!!!!!!!!!!", page_handle.frame->page.page_num, page_handle.frame->pin_count);
  }

  // 找到空闲位置
  return record_page_handler_.insert_record(data, rid);
}

RC RecordFileHandler::insert_record_with_text(const char *data, int record_size, RID *rid)
{
  RC ret = RC::SUCCESS;
  // 找到没有填满的页面
  int page_count = 0;
  if ((ret = disk_buffer_pool_->get_page_count(file_id_, &page_count)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to get page count while inserting record");
    return ret;
  }
    LOG_INFO("page_count: %d", page_count);

    // 分配第一页
    BPPageHandle first_page_handle;
    if ((ret = disk_buffer_pool_->allocate_page(file_id_, &first_page_handle)) != RC::SUCCESS)
    {
      LOG_ERROR("Failed to allocate page while inserting record. file_it:%d, ret:%d",
                file_id_, ret);
      return ret;
    }
    LOG_INFO("!!!!!!!!!!!!!!!!!BEFORE page %d's pin count: %d !!!!!!!!!!!!!!!!", first_page_handle.frame->page.page_num, first_page_handle.frame->pin_count);
    // 第一页存固定数量的数据，先取4000试一下
    const int first_data_size = 4000;
    PageNum first_page_num = first_page_handle.frame->page.page_num;
    record_page_handler_.deinit();
    ret = record_page_handler_.init_empty_page(*disk_buffer_pool_, file_id_, first_page_num, first_data_size);
    if (ret != RC::SUCCESS)
    {
      LOG_ERROR("Failed to init empty page. file_id:%d, ret:%d", file_id_, ret);
      if (RC::SUCCESS != disk_buffer_pool_->unpin_page(&first_page_handle))
      {
        LOG_ERROR("Failed to unpin page. file_id:%d", file_id_);
      }
      return ret;
    }
    if (RC::SUCCESS != disk_buffer_pool_->unpin_page(&first_page_handle))
    {
      LOG_ERROR("Failed to unpin page. file_id:%d", file_id_);
    }
    LOG_INFO("!!!!!!!!!!!!!!!!! AFTER page %d's pin count: %d !!!!!!!!!!!!!!!!", first_page_handle.frame->page.page_num, first_page_handle.frame->pin_count);
    char *first_data = new char[first_data_size];
    memset(first_data, 0, first_data_size);
    memcpy(first_data, data, first_data_size);
    ret = record_page_handler_.insert_record(first_data, rid);
    if (ret != RC::SUCCESS) {
        LOG_ERROR("Insert first half failed!");
    }
    // 第一页的下一页标记置为1
    record_page_handler_.page_header_->has_next = 1;
    
    // 分配第二页
    BPPageHandle second_page_handle;
    if ((ret = disk_buffer_pool_->allocate_page(file_id_, &second_page_handle)) != RC::SUCCESS)
    {
      LOG_ERROR("Failed to allocate page while inserting record. file_it:%d, ret:%d",
                file_id_, ret);
      return ret;
    }
    LOG_INFO("!!!!!!!!!!!!!!!!!BEFORE page %d's pin count: %d !!!!!!!!!!!!!!!!", second_page_handle.frame->page.page_num, second_page_handle.frame->pin_count);
    PageNum second_page_num = second_page_handle.frame->page.page_num;
    LOG_INFO("!!!!!!!!!!!!!!!!!!!!!!!!!!!INSERT!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    LOG_INFO("first: %d", first_page_num);
    LOG_INFO("second: %d", second_page_num);
    LOG_INFO("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    // 设置下一页的页号
    record_page_handler_.page_header_->next_page_num = second_page_num;

    // const int second_data_size = record_size - first_data_size;
    const int second_data_size = first_data_size;
    LOG_INFO("first_data_size: %d", first_data_size);
    LOG_INFO("second_data_size: %d", second_data_size);
    LOG_INFO("total size: %d", first_data_size + second_data_size);
    record_page_handler_.deinit();
    ret = record_page_handler_.init_empty_page(*disk_buffer_pool_, file_id_, second_page_num, second_data_size);
    if (ret != RC::SUCCESS)
    {
      LOG_ERROR("Failed to init empty page. file_id:%d, ret:%d", file_id_, ret);
      if (RC::SUCCESS != disk_buffer_pool_->unpin_page(&second_page_handle))
      {
        LOG_ERROR("Failed to unpin page. file_id:%d", file_id_);
      }
      return ret;
    }
    if (RC::SUCCESS != disk_buffer_pool_->unpin_page(&second_page_handle))
    {
      LOG_ERROR("Failed to unpin page. file_id:%d", file_id_);
    }

    LOG_INFO("!!!!!!!!!!!!!!!!! AFTER page %d's pin count: %d !!!!!!!!!!!!!!!!", second_page_handle.frame->page.page_num, second_page_handle.frame->pin_count);

    char *second_data = new char[second_data_size];
    memset(second_data, 0, second_data_size);
    memcpy(second_data, data + first_data_size, second_data_size);
    RID tmp_rid;
    return record_page_handler_.insert_record(second_data, &tmp_rid);
}

RC RecordFileHandler::update_record(const Record *rec)
{

  RC ret = RC::SUCCESS;

  RecordPageHandler page_handler;
  if ((ret != page_handler.init(*disk_buffer_pool_, file_id_, rec->rid.page_num)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to init record page handler.page number=%d, file_id=%d",
              rec->rid.page_num, file_id_);
    return ret;
  }

  return page_handler.update_record(rec);
}

RC RecordFileHandler::delete_record(const RID *rid)
{

  RC ret = RC::SUCCESS;
  RecordPageHandler page_handler;
  // 这里会调用get_this_page，从而使pin_count + 1
  if ((ret != page_handler.init(*disk_buffer_pool_, file_id_, rid->page_num)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to init record page handler.page number=%d, file_id:%d",
              rid->page_num, file_id_);
    return ret;
  }
  LOG_INFO("DELETE: page %d's pin count: %d!!!!!!!!!!!!!!!!!!!!!!!!!!", page_handler.page_handle_.frame->pin_count);
  if (page_handler.page_header_->has_next == 1) {
      // 前面已经init过一次了，需要deinit一下
      page_handler.deinit();
      return delete_record_with_text(rid);
  }
  return page_handler.delete_record(rid);
}

RC RecordFileHandler::delete_record_with_text(const RID *rid)
{
  RC ret = RC::SUCCESS;
  // 这里应该已经init过了，不需要再一次init
  RecordPageHandler page_handler;
  if ((ret != page_handler.init(*disk_buffer_pool_, file_id_, rid->page_num)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to init record page handler.page number=%d, file_id:%d",
              rid->page_num, file_id_);
    return ret;
  }
  LOG_INFO("DELETE: page %d's pin count: %d!!!!!!!!!!!!!!!!!!!!!!!!!!", page_handler.page_handle_.frame->pin_count);
  assert(page_handler.page_header_->has_next == 1);
  // 删除第一页前先保存next_page_num
  PageNum next_page_num = page_handler.page_header_->next_page_num;
  ret = page_handler.delete_record(rid);
  if (ret != RC::SUCCESS) {
      LOG_ERROR("Delete first page failed!");
      return ret;
  }
  // 删除第二页
  // PageNum next_page_num = rid->page_num + 1;
  
  // 切换page_handler
  LOG_INFO("DELETE: page %d's pin count: %d!!!!!!!!!!!!!!!!!!!!!!!!!!", page_handler.page_handle_.frame->pin_count);
  page_handler.deinit();  
  if ((ret != page_handler.init(*disk_buffer_pool_, file_id_, next_page_num)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to init record page handler.page number=%d, file_id:%d",
              next_page_num, file_id_);
    return ret;
  }
  LOG_INFO("DELETE: page %d's pin count: %d!!!!!!!!!!!!!!!!!!!!!!!!!!", page_handler.page_handle_.frame->pin_count);
  RID tmp_rid;
  tmp_rid.page_num = next_page_num;
  tmp_rid.slot_num = 0;
  return page_handler.delete_record(&tmp_rid);
}

RC RecordFileHandler::get_record(const RID *rid, Record *rec)
{
  // lock?
  RC ret = RC::SUCCESS;
  if (nullptr == rid || nullptr == rec)
  {
    LOG_ERROR("Invalid rid %p or rec %p, one of them is null. ", rid, rec);
    return RC::INVALID_ARGUMENT;
  }
  RecordPageHandler page_handler;
  if ((ret != page_handler.init(*disk_buffer_pool_, file_id_, rid->page_num)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to init record page handler.page number=%d, file_id:%d",
              rid->page_num, file_id_);
    return ret;
  }
  return page_handler.get_record(rid, rec);
}

////////////////////////////////////////////////////////////////////////////////

RecordFileScanner::RecordFileScanner() : disk_buffer_pool_(nullptr),
                                         file_id_(-1),
                                         condition_filter_(nullptr)
{
}

RC RecordFileScanner::open_scan(DiskBufferPool &buffer_pool, int file_id, ConditionFilter *condition_filter)
{
  close_scan();

  disk_buffer_pool_ = &buffer_pool;
  file_id_ = file_id;

  condition_filter_ = condition_filter;
  return RC::SUCCESS;
}

RC RecordFileScanner::close_scan()
{
  if (disk_buffer_pool_ != nullptr)
  {
    disk_buffer_pool_ = nullptr;
  }

  if (condition_filter_ != nullptr)
  {
    condition_filter_ = nullptr;
  }

  return RC::SUCCESS;
}

RC RecordFileScanner::get_first_record(Record *rec, bool& has_text)
{
    RC ret;
    int page_count = 0;
  if ((ret = disk_buffer_pool_->get_page_count(file_id_, &page_count)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to get page count while getting next record. file id=%d", file_id_);
    return RC::RECORD_EOF;
  }
  scanned_.clear();
  for (int i = 0; i < page_count; i++) {
      scanned_.push_back(false);
  }
  
  rec->rid.page_num = 1; // from 1 参考DiskBufferPool
  rec->rid.slot_num = -1;
  // rec->valid = false;
  return get_next_record(rec, has_text);
}

PageNum find_next_scan_page(std::vector<bool>& scanned)
{
    int len = scanned.size();
    for (int i = 1; i < len; i++) {
        if (scanned[i] == false) {
            return i;
        }
    }
    return -1;
}

RC RecordFileScanner::get_next_record_with_text(Record *rec, bool& has_text)
{
//   if (nullptr == disk_buffer_pool_)
//   {
//     LOG_ERROR("Scanner has been closed.");
//     return RC::RECORD_CLOSED;
//   }

//   RC ret = RC::SUCCESS;
//   Record current_record = *rec;

// while ()
//   {

//       // 切换record_page_handler_
//     if (current_record.rid.page_num != record_page_handler_.get_page_num())
//     {
//       record_page_handler_.deinit();
//       ret = record_page_handler_.init(*disk_buffer_pool_, file_id_, current_record.rid.page_num);
//       if (ret != RC::SUCCESS && ret != RC::BUFFERPOOL_INVALID_PAGE_NUM)
//       {
//         LOG_ERROR("Failed to init record page handler. page num=%d", current_record.rid.page_num);
//         return ret;
//       }

//       if (RC::BUFFERPOOL_INVALID_PAGE_NUM == ret)
//       {
//         current_record.rid.page_num++;
//         current_record.rid.slot_num = -1;
//         continue;
//       }
//     }

//     ret = record_page_handler_.get_next_record(&current_record);

//     // TEXT
//         if (record_page_handler_.page_header_->has_next == 1){
//           // LOG_INFO("has next!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
//           PageNum first_page_num = current_record.rid.page_num;
//           scanned[first_page_num] = true;
//           // 保存第一页的数据
//           const char *first_data = current_record.data;
//           int first_page_real_size = record_page_handler_.page_header_->record_real_size;
//           RID first_rid = current_record.rid;
         
          
//           // 第二页页号
//           PageNum second_page_num = record_page_handler_.page_header_->next_page_num;
//           scanned[second_page_num] = true;
//           // 切换record_page_handler_
//           record_page_handler_.deinit();
//           ret = record_page_handler_.init(*disk_buffer_pool_, file_id_, second_page_num);
//           if (ret != RC::SUCCESS && ret != RC::BUFFERPOOL_INVALID_PAGE_NUM)
//           {
//               LOG_ERROR("Failed to init record page handler. page num=%d", second_page_num);
//               return ret;
//           }

//           // current_record.rid.page_num = first_rid.page_num + 1; // 假设两页是连续的
//           current_record.rid.slot_num = -1;
//           // 取下一页的数据
//           ret = record_page_handler_.get_next_record(&current_record);

//           // 保存第二页数据
//           int second_page_real_size = record_page_handler_.page_header_->record_real_size;
//           const char *second_data = current_record.data;
//           RID second_rid = current_record.rid;

//           // 拼接两页的数据
//         //   LOG_INFO("first_page_real_size: %d", first_page_real_size);
//         //   LOG_INFO("second_page_real_size: %d", second_page_real_size);
//         //   LOG_INFO("total size: %d", first_page_real_size + second_page_real_size);

//           char *final_data = new char[first_page_real_size + second_page_real_size];
//           // memset(final_data, 0, first_page_real_size + second_page_real_size);
//           memcpy(final_data, first_data, first_page_real_size);
//           memcpy(final_data + first_page_real_size, second_data, second_page_real_size);

          
//           final_record.data = final_data;
//           // 这里的rid要置为第二页的RID，因为后面的数据需要在此页的基础上找
//           final_record.rid = second_rid;

//           has_text = true;

//           if (ret == RC::SUCCESS) {
//               if (condition_filter_ == nullptr || condition_filter_->filter(final_record)) {
//                 text = true;
//                     LOG_INFO("!!!!!!!!!!!!!!!!!!!!!!!!!!!SCAN!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
//     LOG_INFO("first: %d", first_page_num);
//     LOG_INFO("second: %d", second_page_num);
//     LOG_INFO("page num: %d", page_count);
//     LOG_INFO("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
//                 break;
//               }
//           } else if (RC::RECORD_EOF == ret) {
//                 PageNum num = find_next_scan_page(scanned);
//                 LOG_INFO("next scanned page num: %d!!!!!!!!!!!!", num);
//                 if (num == -1) {
//                     break;
//                 }
//                 current_record.rid.page_num = num;
//                 current_record.rid.slot_num = -1;
//           } else {
//               break; // ERROR
//           }
//       }
    
//   } // while 

//   if (RC::SUCCESS == ret)
//   {
//           *rec = final_record;

//   }

//   return ret;
}

// rec同时作为入参和出参，入参时是上一条记录，出参是下一条记录
RC RecordFileScanner::get_next_record(Record *rec, bool& has_text)
{
  if (nullptr == disk_buffer_pool_)
  {
    LOG_ERROR("Scanner has been closed.");
    return RC::RECORD_CLOSED;
  }

  RC ret = RC::SUCCESS;
  Record current_record = *rec;

  int page_count = 0;
  if ((ret = disk_buffer_pool_->get_page_count(file_id_, &page_count)) != RC::SUCCESS)
  {
    LOG_ERROR("Failed to get page count while getting next record. file id=%d", file_id_);
    return RC::RECORD_EOF;
  }

  if (1 == page_count)
  {
    return RC::RECORD_EOF;
  }

  bool text = false;
  Record final_record;
   // std::vector<bool> scanned(page_count + 1, false); // 标记每一页是否扫描过
  while (current_record.rid.page_num < page_count)
  {

      // 切换record_page_handler_
    if (current_record.rid.page_num != record_page_handler_.get_page_num())
    {
      record_page_handler_.deinit();
      ret = record_page_handler_.init(*disk_buffer_pool_, file_id_, current_record.rid.page_num);
      if (ret != RC::SUCCESS && ret != RC::BUFFERPOOL_INVALID_PAGE_NUM)
      {
          scanned_[current_record.rid.page_num] = true;
        LOG_ERROR("Failed to init record page handler. page num=%d", current_record.rid.page_num);
        return ret;
      }

      if (RC::BUFFERPOOL_INVALID_PAGE_NUM == ret)
      {
        LOG_INFO("BUFFERPOOL_INVALID_PAGE_NUM!!!!!!!!!!!!!");
        scanned_[current_record.rid.page_num] = true;
        current_record.rid.page_num++;
        // 删除TEXT的页后
        current_record.rid.slot_num = -1;
        continue;
      }
    }

    ret = record_page_handler_.get_next_record(&current_record);

    // TEXT 
        if (record_page_handler_.page_header_->has_next == 1) {
          // LOG_INFO("has next!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
          PageNum first_page_num = current_record.rid.page_num;
          scanned_[first_page_num] = true;
          // 保存第一页的数据
          const char *first_data = current_record.data;
          int first_page_real_size = record_page_handler_.page_header_->record_real_size;
          RID first_rid = current_record.rid;
         
          
          // 第二页页号
          PageNum second_page_num = record_page_handler_.page_header_->next_page_num;
          scanned_[second_page_num] = true;
          // 切换record_page_handler_
          record_page_handler_.deinit();
          ret = record_page_handler_.init(*disk_buffer_pool_, file_id_, second_page_num);
          if (ret != RC::SUCCESS && ret != RC::BUFFERPOOL_INVALID_PAGE_NUM)
          {
              LOG_ERROR("Failed to init record page handler. page num=%d", second_page_num);
              return ret;
          }

          // current_record.rid.page_num = first_rid.page_num + 1; // 假设两页是连续的
          current_record.rid.slot_num = -1;
          // 取下一页的数据
          ret = record_page_handler_.get_next_record(&current_record);

          // 保存第二页数据
          int second_page_real_size = record_page_handler_.page_header_->record_real_size;
          const char *second_data = current_record.data;
          RID second_rid = current_record.rid;

          // 拼接两页的数据
        //   LOG_INFO("first_page_real_size: %d", first_page_real_size);
        //   LOG_INFO("second_page_real_size: %d", second_page_real_size);
        //   LOG_INFO("total size: %d", first_page_real_size + second_page_real_size);

          char *final_data = new char[first_page_real_size + second_page_real_size];
          // memset(final_data, 0, first_page_real_size + second_page_real_size);
          memcpy(final_data, first_data, first_page_real_size);
          memcpy(final_data + first_page_real_size, second_data, second_page_real_size);

          
          final_record.data = final_data;
          // 这里的rid要置为第二页的RID，因为后面的数据需要在此页的基础上找
          final_record.rid = second_rid;
          
          has_text = true;

          if (ret == RC::SUCCESS) {
              if (condition_filter_ == nullptr || condition_filter_->filter(final_record)) {
                text = true;
    LOG_INFO("!!!!!!!!!!!!!!!!!!!!!!!!!!!SCAN!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
    LOG_INFO("first: %d", first_page_num);
    LOG_INFO("second: %d", second_page_num);
    LOG_INFO("page num: %d", page_count);
    LOG_INFO("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
                break;
              }
          } else if (RC::RECORD_EOF == ret) {
                current_record.rid.page_num++;
                current_record.rid.slot_num = -1;
          } else {
              break; // ERROR
          }
          
          // return ret;
    }
    ////////////////
  else { // 非TEXT
    if (RC::SUCCESS == ret) {
      if (condition_filter_ == nullptr || condition_filter_->filter(current_record)) {
        break; // got one
      }
    } else if (RC::RECORD_EOF == ret)  // TEXT的第二页同样走这里
    {
        if (has_text == true) {
            PageNum num = find_next_scan_page(scanned_);
            LOG_INFO("find_next_scan_page: %d", num);
            if (num == -1) {
                return RC::RECORD_EOF;
            }
            current_record.rid.page_num = num;
            current_record.rid.slot_num = -1;
        } else {
            scanned_[current_record.rid.page_num] = true;
            current_record.rid.page_num++;
            current_record.rid.slot_num = -1;
        }
    } else {
      break; // ERROR
    }
  } 
  } // while 

  if (RC::SUCCESS == ret)
  {
      if (text == true) {
          *rec = final_record;
      } else {
        *rec = current_record;
      }
  }

  return ret;
}