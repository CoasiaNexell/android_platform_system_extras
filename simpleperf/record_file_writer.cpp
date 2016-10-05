/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "record_file.h"

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>

#include "event_attr.h"
#include "perf_event.h"
#include "record.h"
#include "utils.h"

using namespace PerfFileFormat;

std::unique_ptr<RecordFileWriter> RecordFileWriter::CreateInstance(const std::string& filename) {
  // Remove old perf.data to avoid file ownership problems.
  std::string err;
  if (!android::base::RemoveFileIfExists(filename, &err)) {
    LOG(ERROR) << "failed to remove file " << filename << ": " << err;
    return nullptr;
  }
  FILE* fp = fopen(filename.c_str(), "web+");
  if (fp == nullptr) {
    PLOG(ERROR) << "failed to open record file '" << filename << "'";
    return nullptr;
  }

  return std::unique_ptr<RecordFileWriter>(new RecordFileWriter(filename, fp));
}

RecordFileWriter::RecordFileWriter(const std::string& filename, FILE* fp)
    : filename_(filename),
      record_fp_(fp),
      attr_section_offset_(0),
      attr_section_size_(0),
      data_section_offset_(0),
      data_section_size_(0),
      feature_count_(0),
      current_feature_index_(0) {
}

RecordFileWriter::~RecordFileWriter() {
  if (record_fp_ != nullptr) {
    Close();
  }
}

bool RecordFileWriter::WriteAttrSection(const std::vector<EventAttrWithId>& attr_ids) {
  if (attr_ids.empty()) {
    return false;
  }

  // Skip file header part.
  if (fseek(record_fp_, sizeof(FileHeader), SEEK_SET) == -1) {
    return false;
  }

  // Write id section.
  off_t id_section_offset = ftello(record_fp_);
  if (id_section_offset == -1) {
    return false;
  }
  for (auto& attr_id : attr_ids) {
    if (!Write(attr_id.ids.data(), attr_id.ids.size() * sizeof(uint64_t))) {
      return false;
    }
  }

  // Write attr section.
  off_t attr_section_offset = ftello(record_fp_);
  if (attr_section_offset == -1) {
    return false;
  }
  for (auto& attr_id : attr_ids) {
    FileAttr file_attr;
    file_attr.attr = *attr_id.attr;
    file_attr.ids.offset = id_section_offset;
    file_attr.ids.size = attr_id.ids.size() * sizeof(uint64_t);
    id_section_offset += file_attr.ids.size;
    if (!Write(&file_attr, sizeof(file_attr))) {
      return false;
    }
  }

  off_t data_section_offset = ftello(record_fp_);
  if (data_section_offset == -1) {
    return false;
  }

  attr_section_offset_ = attr_section_offset;
  attr_section_size_ = data_section_offset - attr_section_offset;
  data_section_offset_ = data_section_offset;

  // Save event_attr for use when reading records.
  event_attr_ = *attr_ids[0].attr;
  return true;
}

bool RecordFileWriter::WriteRecord(const Record& record) {
  // linux-tools-perf only accepts records with size <= 65535 bytes. To make
  // perf.data generated by simpleperf be able to be parsed by linux-tools-perf,
  // Split simpleperf custom records which are > 65535 into a bunch of
  // RECORD_SPLIT records, followed by a RECORD_SPLIT_END record.
  constexpr uint32_t RECORD_SIZE_LIMIT = 65535;
  if (record.size() <= RECORD_SIZE_LIMIT) {
    WriteData(record.Binary(), record.size());
    return true;
  }
  CHECK_GT(record.type(), SIMPLE_PERF_RECORD_TYPE_START);
  const char* p = record.Binary();
  uint32_t left_bytes = static_cast<uint32_t>(record.size());
  RecordHeader header;
  header.type = SIMPLE_PERF_RECORD_SPLIT;
  char header_buf[Record::header_size()];
  char* header_p;
  while (left_bytes > 0) {
    uint32_t bytes_to_write = std::min(RECORD_SIZE_LIMIT - Record::header_size(), left_bytes);
    header.size = bytes_to_write + Record::header_size();
    header_p = header_buf;
    header.MoveToBinaryFormat(header_p);
    if (!WriteData(header_buf, Record::header_size())) {
      return false;
    }
    if (!WriteData(p, bytes_to_write)) {
      return false;
    }
    p += bytes_to_write;
    left_bytes -= bytes_to_write;
  }
  header.type = SIMPLE_PERF_RECORD_SPLIT_END;
  header.size = Record::header_size();
  header_p = header_buf;
  header.MoveToBinaryFormat(header_p);
  return WriteData(header_buf, Record::header_size());
}

bool RecordFileWriter::WriteData(const void* buf, size_t len) {
  if (!Write(buf, len)) {
    return false;
  }
  data_section_size_ += len;
  return true;
}

bool RecordFileWriter::Write(const void* buf, size_t len) {
  if (fwrite(buf, len, 1, record_fp_) != 1) {
    PLOG(ERROR) << "failed to write to record file '" << filename_ << "'";
    return false;
  }
  return true;
}

std::unique_ptr<Record> RecordFileWriter::ReadRecordFromFile(FILE* fp, std::vector<char>& buf) {
  if (buf.size() < sizeof(perf_event_header)) {
    buf.resize(sizeof(perf_event_header));
  }
  auto pheader = reinterpret_cast<perf_event_header*>(buf.data());
  if (fread(pheader, sizeof(*pheader), 1, fp) != 1) {
    PLOG(ERROR) << "read failed";
    return nullptr;
  }
  if (pheader->size > sizeof(*pheader)) {
    if (pheader->size > buf.size()) {
      buf.resize(pheader->size);
    }
    pheader = reinterpret_cast<perf_event_header*>(buf.data());
    if (fread(pheader + 1, pheader->size - sizeof(*pheader), 1, fp) != 1) {
      PLOG(ERROR) << "read failed";
      return nullptr;
    }
  }
  return ReadRecordFromBuffer(event_attr_, pheader->type, buf.data());
}

bool RecordFileWriter::WriteRecordToFile(FILE* fp, std::unique_ptr<Record> r) {
  if (fwrite(r->Binary(), r->size(), 1, fp) != 1) {
    PLOG(ERROR) << "write failed";
    return false;
  }
  return true;
}

// SortDataSection() sorts records in data section in time order.
// This method is suitable for the situation that there is only one buffer
// between kernel and simpleperf for each cpu. The order of records in each
// cpu buffer is already sorted, so we only need to merge records from different
// cpu buffers.
// 1. Create one temporary file for each cpu, and write records to different
//    temporary files according to their cpu value.
// 2. Use RecordCache to merge records from different temporary files.
bool RecordFileWriter::SortDataSection() {
  if (!IsTimestampSupported(event_attr_) || !IsCpuSupported(event_attr_)) {
    // Omit the sort if either timestamp or cpu is not recorded.
    return true;
  }
  struct CpuData {
    std::string path;
    FILE* fp;
    std::vector<char> buf;
    uint64_t data_size;

    explicit CpuData(const std::string& path) : path(path), fp(nullptr), data_size(0) {
      fp = fopen(path.c_str(), "web+");
    }
    ~CpuData() {
      fclose(fp);
      unlink(path.c_str());
    }
  };
  std::unordered_map<uint32_t, std::unique_ptr<CpuData>> cpu_map;
  if (fseek(record_fp_, data_section_offset_, SEEK_SET) == -1) {
    PLOG(ERROR) << "fseek() failed";
    return false;
  }
  uint64_t cur_size = 0;
  std::vector<char> global_buf;
  while (cur_size < data_section_size_) {
    std::unique_ptr<Record> r = ReadRecordFromFile(record_fp_, global_buf);
    if (r == nullptr) {
      return false;
    }
    cur_size += r->size();
    std::unique_ptr<CpuData>& cpu_data = cpu_map[r->Cpu()];
    if (cpu_data == nullptr) {
      // Create temporary file in the same directory as filename_, because we
      // may not have permission to create temporary file in other directories.
      cpu_data.reset(new CpuData(filename_ + "." + std::to_string(r->Cpu())));
      if (cpu_data->fp == nullptr) {
        PLOG(ERROR) << "failed to open tmpfile " << cpu_data->path;
        return false;
      }
    }
    cpu_data->data_size += r->size();
    if (!WriteRecordToFile(cpu_data->fp, std::move(r))) {
      return false;
    }
  }
  if (fseek(record_fp_, data_section_offset_, SEEK_SET) == -1) {
    PLOG(ERROR) << "fseek() failed";
    return false;
  }
  RecordCache global_cache(true);
  for (auto it = cpu_map.begin(); it != cpu_map.end(); ++it) {
    if (fseek(it->second->fp, 0, SEEK_SET) == -1) {
      PLOG(ERROR) << "fseek() failed";
      return false;
    }
    std::unique_ptr<Record> r = ReadRecordFromFile(it->second->fp, it->second->buf);
    if (r == nullptr) {
      return false;
    }
    it->second->data_size -= r->size();
    global_cache.Push(std::move(r));
  }
  while (true) {
    std::unique_ptr<Record> r = global_cache.ForcedPop();
    if (r == nullptr) {
      break;
    }
    uint32_t cpu = r->Cpu();
    if (!WriteRecordToFile(record_fp_, std::move(r))) {
      return false;
    }
    // Each time writing one record of a cpu, push the next record from the
    // temporary file belong to that cpu into the record cache.
    std::unique_ptr<CpuData>& cpu_data = cpu_map[cpu];
    if (cpu_data->data_size > 0) {
      r = ReadRecordFromFile(cpu_data->fp, cpu_data->buf);
      if (r == nullptr) {
        return false;
      }
      cpu_data->data_size -= r->size();
      global_cache.Push(std::move(r));
    }
  }
  return true;
}

bool RecordFileWriter::SeekFileEnd(uint64_t* file_end) {
  if (fseek(record_fp_, 0, SEEK_END) == -1) {
    PLOG(ERROR) << "fseek() failed";
    return false;
  }
  off_t offset = ftello(record_fp_);
  if (offset == -1) {
    PLOG(ERROR) << "ftello() failed";
    return false;
  }
  *file_end = static_cast<uint64_t>(offset);
  return true;
}

bool RecordFileWriter::WriteFeatureHeader(size_t feature_count) {
  feature_count_ = feature_count;
  current_feature_index_ = 0;
  uint64_t feature_header_size = feature_count * sizeof(SectionDesc);

  // Reserve enough space in the record file for the feature header.
  std::vector<unsigned char> zero_data(feature_header_size);
  if (fseek(record_fp_, data_section_offset_ + data_section_size_, SEEK_SET) == -1) {
    PLOG(ERROR) << "fseek() failed";
    return false;
  }
  return Write(zero_data.data(), zero_data.size());
}

bool RecordFileWriter::WriteBuildIdFeature(const std::vector<BuildIdRecord>& build_id_records) {
  uint64_t start_offset;
  if (!WriteFeatureBegin(&start_offset)) {
    return false;
  }
  for (auto& record : build_id_records) {
    if (!Write(record.Binary(), record.size())) {
      return false;
    }
  }
  return WriteFeatureEnd(FEAT_BUILD_ID, start_offset);
}

bool RecordFileWriter::WriteFeatureString(int feature, const std::string& s) {
  uint64_t start_offset;
  if (!WriteFeatureBegin(&start_offset)) {
    return false;
  }
  uint32_t len = static_cast<uint32_t>(Align(s.size() + 1, 64));
  if (!Write(&len, sizeof(len))) {
    return false;
  }
  std::vector<char> v(len, '\0');
  std::copy(s.begin(), s.end(), v.begin());
  if (!Write(v.data(), v.size())) {
    return false;
  }
  return WriteFeatureEnd(feature, start_offset);
}

bool RecordFileWriter::WriteCmdlineFeature(const std::vector<std::string>& cmdline) {
  uint64_t start_offset;
  if (!WriteFeatureBegin(&start_offset)) {
    return false;
  }
  uint32_t arg_count = cmdline.size();
  if (!Write(&arg_count, sizeof(arg_count))) {
    return false;
  }
  for (auto& arg : cmdline) {
    uint32_t len = static_cast<uint32_t>(Align(arg.size() + 1, 64));
    if (!Write(&len, sizeof(len))) {
      return false;
    }
    std::vector<char> array(len, '\0');
    std::copy(arg.begin(), arg.end(), array.begin());
    if (!Write(array.data(), array.size())) {
      return false;
    }
  }
  return WriteFeatureEnd(FEAT_CMDLINE, start_offset);
}

bool RecordFileWriter::WriteBranchStackFeature() {
  uint64_t start_offset;
  if (!WriteFeatureBegin(&start_offset)) {
    return false;
  }
  return WriteFeatureEnd(FEAT_BRANCH_STACK, start_offset);
}

bool RecordFileWriter::WriteFeatureBegin(uint64_t* start_offset) {
  CHECK_LT(current_feature_index_, feature_count_);
  if (!SeekFileEnd(start_offset)) {
    return false;
  }
  return true;
}

bool RecordFileWriter::WriteFeatureEnd(int feature, uint64_t start_offset) {
  uint64_t end_offset;
  if (!SeekFileEnd(&end_offset)) {
    return false;
  }
  SectionDesc desc;
  desc.offset = start_offset;
  desc.size = end_offset - start_offset;
  uint64_t feature_offset = data_section_offset_ + data_section_size_;
  if (fseek(record_fp_, feature_offset + current_feature_index_ * sizeof(SectionDesc), SEEK_SET) ==
      -1) {
    PLOG(ERROR) << "fseek() failed";
    return false;
  }
  if (!Write(&desc, sizeof(SectionDesc))) {
    return false;
  }
  ++current_feature_index_;
  features_.push_back(feature);
  return true;
}

bool RecordFileWriter::WriteFileHeader() {
  FileHeader header;
  memset(&header, 0, sizeof(header));
  memcpy(header.magic, PERF_MAGIC, sizeof(header.magic));
  header.header_size = sizeof(header);
  header.attr_size = sizeof(FileAttr);
  header.attrs.offset = attr_section_offset_;
  header.attrs.size = attr_section_size_;
  header.data.offset = data_section_offset_;
  header.data.size = data_section_size_;
  for (auto& feature : features_) {
    int i = feature / 8;
    int j = feature % 8;
    header.features[i] |= (1 << j);
  }

  if (fseek(record_fp_, 0, SEEK_SET) == -1) {
    return false;
  }
  if (!Write(&header, sizeof(header))) {
    return false;
  }
  return true;
}

bool RecordFileWriter::Close() {
  CHECK(record_fp_ != nullptr);
  bool result = true;

  // Write file header. We gather enough information to write file header only after
  // writing data section and feature section.
  if (!WriteFileHeader()) {
    result = false;
  }

  if (fclose(record_fp_) != 0) {
    PLOG(ERROR) << "failed to close record file '" << filename_ << "'";
    result = false;
  }
  record_fp_ = nullptr;
  return result;
}
