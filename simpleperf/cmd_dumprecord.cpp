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

#include <inttypes.h>

#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include "command.h"
#include "dso.h"
#include "ETMDecoder.h"
#include "event_attr.h"
#include "event_type.h"
#include "perf_regs.h"
#include "record.h"
#include "record_file.h"
#include "tracing.h"
#include "utils.h"

using namespace PerfFileFormat;
using namespace simpleperf;

namespace {

struct SymbolInfo {
  Dso* dso;
  const Symbol* symbol;
  uint64_t vaddr_in_file;
};

using ExtractFieldFn = std::function<std::string(const TracingField&, const char*)>;

struct EventInfo {
  size_t tp_data_size = 0;
  std::vector<TracingField> tp_fields;
  std::vector<ExtractFieldFn> extract_field_functions;
};

std::string ExtractStringField(const TracingField& field, const char* data) {
  std::string s;
  // data points to a char [field.elem_count] array. It is not guaranteed to be ended
  // with '\0'. So need to copy from data like strncpy.
  for (size_t i = 0; i < field.elem_count && data[i] != '\0'; i++) {
    s.push_back(data[i]);
  }
  return s;
}

template <typename T, typename UT = typename std::make_unsigned<T>::type>
std::string ExtractIntField(const TracingField& field, const char* data) {
  static_assert(std::is_signed<T>::value);

  T value;
  MoveFromBinaryFormat(value, data);

  if (field.is_signed) {
    return android::base::StringPrintf("%" PRId64, static_cast<int64_t>(value));
  }
  return android::base::StringPrintf("0x%" PRIx64, static_cast<uint64_t>(static_cast<UT>(value)));
}

template <typename T>
std::string ExtractIntArrayField(const TracingField& field, const char* data) {
  std::string s;
  for (size_t i = 0; i < field.elem_count; i++) {
    if (i != 0) {
      s.push_back(' ');
    }
    s += ExtractIntField<T>(field, data);
    data += field.elem_size;
  }
  return s;
}

std::string ExtractUnknownField(const TracingField& field, const char* data) {
  size_t total = field.elem_size * field.elem_count;
  uint32_t value;
  std::string s;
  for (size_t i = 0; i + sizeof(value) <= total; i += sizeof(value)) {
    if (i != 0) {
      s.push_back(' ');
    }
    MoveFromBinaryFormat(value, data);
    s += android::base::StringPrintf("0x%08x", value);
  }
  return s;
}

ExtractFieldFn GetExtractFieldFunction(const TracingField& field) {
  if (field.elem_count > 1 && field.elem_size == 1) {
    // Probably the field is a string.
    // Don't use field.is_signed, which has different values on x86 and arm.
    return ExtractStringField;
  }
  if (field.elem_count == 1) {
    switch (field.elem_size) {
      case 1: return ExtractIntField<int8_t>;
      case 2: return ExtractIntField<int16_t>;
      case 4: return ExtractIntField<int32_t>;
      case 8: return ExtractIntField<int64_t>;
    }
  } else {
    switch (field.elem_size) {
      case 1: return ExtractIntArrayField<int8_t>;
      case 2: return ExtractIntArrayField<int16_t>;
      case 4: return ExtractIntArrayField<int32_t>;
      case 8: return ExtractIntArrayField<int64_t>;
    }
  }
  return ExtractUnknownField;
}

class DumpRecordCommand : public Command {
 public:
  DumpRecordCommand()
      : Command("dump", "dump perf record file",
                // clang-format off
"Usage: simpleperf dumprecord [options] [perf_record_file]\n"
"    Dump different parts of a perf record file. Default file is perf.data.\n"
"--dump-etm type1,type2,...   Dump etm data. A type is one of raw, packet and element.\n"
"--symdir <dir>               Look for binaries in a directory recursively.\n"
                // clang-format on
      ) {}

  bool Run(const std::vector<std::string>& args);

 private:
  bool ParseOptions(const std::vector<std::string>& args);
  void DumpFileHeader();
  void DumpAttrSection();
  bool DumpDataSection();
  bool ProcessRecord(Record* r);
  void ProcessSampleRecord(const SampleRecord& r);
  void ProcessCallChainRecord(const CallChainRecord& r);
  SymbolInfo GetSymbolInfo(uint32_t pid, uint32_t tid, uint64_t ip, bool in_kernel);
  void ProcessTracingData(const TracingDataRecord& r);
  bool DumpAuxData(const AuxRecord& aux);
  bool DumpFeatureSection();

  // options
  std::string record_filename_ = "perf.data";
  ETMDumpOption etm_dump_option_;

  std::unique_ptr<RecordFileReader> record_file_reader_;
  std::unique_ptr<ETMDecoder> etm_decoder_;
  ThreadTree thread_tree_;

  std::vector<EventInfo> events_;
};

bool DumpRecordCommand::Run(const std::vector<std::string>& args) {
  if (!ParseOptions(args)) {
    return false;
  }
  record_file_reader_ = RecordFileReader::CreateInstance(record_filename_);
  if (record_file_reader_ == nullptr) {
    return false;
  }
  DumpFileHeader();
  DumpAttrSection();
  if (!DumpDataSection()) {
    return false;
  }
  return DumpFeatureSection();
}

bool DumpRecordCommand::ParseOptions(const std::vector<std::string>& args) {
  size_t i;
  for (i = 0; i < args.size() && !args[i].empty() && args[i][0] == '-'; ++i) {
    if (args[i] == "--dump-etm") {
      if (!NextArgumentOrError(args, &i) || !ParseEtmDumpOption(args[i], &etm_dump_option_)) {
        return false;
      }
    } else if (args[i] == "--symdir") {
      if (!NextArgumentOrError(args, &i)) {
        return false;
      }
      if (!Dso::AddSymbolDir(args[i])) {
        return false;
      }
    } else {
      ReportUnknownOption(args, i);
      return false;
    }
  }
  if (i + 1 < args.size()) {
    LOG(ERROR) << "too many record files";
    return false;
  }
  if (i + 1 == args.size()) {
    record_filename_ = args[i];
  }
  return true;
}

static const std::string GetFeatureNameOrUnknown(int feature) {
  std::string name = GetFeatureName(feature);
  return name.empty() ? android::base::StringPrintf("unknown_feature(%d)", feature) : name;
}

void DumpRecordCommand::DumpFileHeader() {
  const FileHeader& header = record_file_reader_->FileHeader();
  printf("magic: ");
  for (size_t i = 0; i < 8; ++i) {
    printf("%c", header.magic[i]);
  }
  printf("\n");
  printf("header_size: %" PRId64 "\n", header.header_size);
  if (header.header_size != sizeof(header)) {
    PLOG(WARNING) << "record file header size " << header.header_size
                  << "doesn't match expected header size " << sizeof(header);
  }
  printf("attr_size: %" PRId64 "\n", header.attr_size);
  if (header.attr_size != sizeof(FileAttr)) {
    LOG(WARNING) << "record file attr size " << header.attr_size
                  << " doesn't match expected attr size " << sizeof(FileAttr);
  }
  printf("attrs[file section]: offset %" PRId64 ", size %" PRId64 "\n", header.attrs.offset,
         header.attrs.size);
  printf("data[file section]: offset %" PRId64 ", size %" PRId64 "\n", header.data.offset,
         header.data.size);
  printf("event_types[file section]: offset %" PRId64 ", size %" PRId64 "\n",
         header.event_types.offset, header.event_types.size);

  std::vector<int> features;
  for (size_t i = 0; i < FEAT_MAX_NUM; ++i) {
    size_t j = i / 8;
    size_t k = i % 8;
    if ((header.features[j] & (1 << k)) != 0) {
      features.push_back(i);
    }
  }
  for (auto& feature : features) {
    printf("feature: %s\n", GetFeatureNameOrUnknown(feature).c_str());
  }
}

void DumpRecordCommand::DumpAttrSection() {
  std::vector<EventAttrWithId> attrs = record_file_reader_->AttrSection();
  for (size_t i = 0; i < attrs.size(); ++i) {
    const auto& attr = attrs[i];
    printf("attr %zu:\n", i + 1);
    DumpPerfEventAttr(*attr.attr, 1);
    if (!attr.ids.empty()) {
      printf("  ids:");
      for (const auto& id : attr.ids) {
        printf(" %" PRId64, id);
      }
      printf("\n");
    }
  }
}

bool DumpRecordCommand::DumpDataSection() {
  thread_tree_.ShowIpForUnknownSymbol();
  record_file_reader_->LoadBuildIdAndFileFeatures(thread_tree_);

  auto record_callback = [&](std::unique_ptr<Record> r) {
    return ProcessRecord(r.get());
  };
  return record_file_reader_->ReadDataSection(record_callback);
}

bool DumpRecordCommand::ProcessRecord(Record* r) {
  r->Dump();
  thread_tree_.Update(*r);

  bool res = true;
  switch (r->type()) {
    case PERF_RECORD_SAMPLE:
      ProcessSampleRecord(*static_cast<SampleRecord*>(r));
      break;
    case SIMPLE_PERF_RECORD_CALLCHAIN:
      ProcessCallChainRecord(*static_cast<CallChainRecord*>(r));
      break;
    case PERF_RECORD_AUXTRACE_INFO: {
      etm_decoder_ = ETMDecoder::Create(*static_cast<AuxTraceInfoRecord*>(r), thread_tree_);
      if (etm_decoder_) {
        etm_decoder_->EnableDump(etm_dump_option_);
      } else {
        res = false;
      }
      break;
    }
    case PERF_RECORD_AUX: {
      res = DumpAuxData(*static_cast<AuxRecord*>(r));
      break;
    }
    case PERF_RECORD_TRACING_DATA:
    case SIMPLE_PERF_RECORD_TRACING_DATA: {
      ProcessTracingData(*static_cast<TracingDataRecord*>(r));
      break;
    }
  }
  return res;
}

void DumpRecordCommand::ProcessSampleRecord(const SampleRecord& sr) {
  bool in_kernel = sr.InKernel();
  if (sr.sample_type & PERF_SAMPLE_CALLCHAIN) {
    PrintIndented(1, "callchain:\n");
    for (size_t i = 0; i < sr.callchain_data.ip_nr; ++i) {
      if (sr.callchain_data.ips[i] >= PERF_CONTEXT_MAX) {
        if (sr.callchain_data.ips[i] == PERF_CONTEXT_USER) {
          in_kernel = false;
        }
        continue;
      }
      SymbolInfo s =
          GetSymbolInfo(sr.tid_data.pid, sr.tid_data.tid, sr.callchain_data.ips[i], in_kernel);
      PrintIndented(2, "%s (%s[+%" PRIx64 "])\n", s.symbol->DemangledName(), s.dso->Path().c_str(),
                    s.vaddr_in_file);
    }
  }
  // Dump tracepoint fields.
  if (!events_.empty()) {
    size_t attr_index = record_file_reader_->GetAttrIndexOfRecord(&sr);
    auto& event = events_[attr_index];
    if (event.tp_data_size > 0 && sr.raw_data.size >= event.tp_data_size) {
      const char* p = sr.raw_data.data;
      PrintIndented(1, "tracepoint fields:\n");
      for (size_t i = 0; i < event.tp_fields.size(); i++) {
        auto& field = event.tp_fields[i];
        std::string s = event.extract_field_functions[i](field, p);
        PrintIndented(2, "%s: %s\n", field.name.c_str(), s.c_str());
        p += field.elem_count * field.elem_size;
      }
    }
  }
}

void DumpRecordCommand::ProcessCallChainRecord(const CallChainRecord& cr) {
  PrintIndented(1, "callchain:\n");
  for (size_t i = 0; i < cr.ip_nr; ++i) {
    SymbolInfo s = GetSymbolInfo(cr.pid, cr.tid, cr.ips[i], false);
    PrintIndented(2, "%s (%s[+%" PRIx64 "])\n", s.symbol->DemangledName(), s.dso->Path().c_str(),
                  s.vaddr_in_file);
  }
}

SymbolInfo DumpRecordCommand::GetSymbolInfo(uint32_t pid, uint32_t tid, uint64_t ip,
                                             bool in_kernel) {
  ThreadEntry* thread = thread_tree_.FindThreadOrNew(pid, tid);
  const MapEntry* map = thread_tree_.FindMap(thread, ip, in_kernel);
  SymbolInfo info;
  info.symbol = thread_tree_.FindSymbol(map, ip, &info.vaddr_in_file, &info.dso);
  return info;
}

bool DumpRecordCommand::DumpAuxData(const AuxRecord& aux) {
  size_t size = aux.data->aux_size;
  if (size > 0) {
    std::unique_ptr<uint8_t[]> data(new uint8_t[size]);
    if (!record_file_reader_->ReadAuxData(aux.Cpu(), aux.data->aux_offset, data.get(), size)) {
      return false;
    }
    return etm_decoder_->ProcessData(data.get(), size);
  }
  return true;
}

void DumpRecordCommand::ProcessTracingData(const TracingDataRecord& r) {
  Tracing tracing(std::vector<char>(r.data, r.data + r.data_size));
  std::vector<EventAttrWithId> attrs = record_file_reader_->AttrSection();
  events_.resize(attrs.size());
  for (size_t i = 0; i < attrs.size(); i++) {
    auto& attr = attrs[i].attr;
    auto& event = events_[i];

    if (attr->type != PERF_TYPE_TRACEPOINT) {
      continue;
    }
    TracingFormat format = tracing.GetTracingFormatHavingId(attr->config);
    event.tp_fields = format.fields;
    // Decide dump function for each field.
    for (size_t j = 0; j < event.tp_fields.size(); j++) {
      auto& field = event.tp_fields[j];
      event.extract_field_functions.push_back(GetExtractFieldFunction(field));
      event.tp_data_size += field.elem_count * field.elem_size;
    }
  }
}

bool DumpRecordCommand::DumpFeatureSection() {
  std::map<int, SectionDesc> section_map = record_file_reader_->FeatureSectionDescriptors();
  for (const auto& pair : section_map) {
    int feature = pair.first;
    const auto& section = pair.second;
    printf("feature section for %s: offset %" PRId64 ", size %" PRId64 "\n",
           GetFeatureNameOrUnknown(feature).c_str(), section.offset, section.size);
    if (feature == FEAT_BUILD_ID) {
      std::vector<BuildIdRecord> records = record_file_reader_->ReadBuildIdFeature();
      for (auto& r : records) {
        r.Dump(1);
      }
    } else if (feature == FEAT_OSRELEASE) {
      std::string s = record_file_reader_->ReadFeatureString(feature);
      PrintIndented(1, "osrelease: %s\n", s.c_str());
    } else if (feature == FEAT_ARCH) {
      std::string s = record_file_reader_->ReadFeatureString(feature);
      PrintIndented(1, "arch: %s\n", s.c_str());
    } else if (feature == FEAT_CMDLINE) {
      std::vector<std::string> cmdline = record_file_reader_->ReadCmdlineFeature();
      PrintIndented(1, "cmdline: %s\n", android::base::Join(cmdline, ' ').c_str());
    } else if (feature == FEAT_FILE) {
      std::string file_path;
      uint32_t file_type;
      uint64_t min_vaddr;
      uint64_t file_offset_of_min_vaddr;
      std::vector<Symbol> symbols;
      std::vector<uint64_t> dex_file_offsets;
      size_t read_pos = 0;
      PrintIndented(1, "file:\n");
      while (record_file_reader_->ReadFileFeature(read_pos, &file_path, &file_type,
                                                  &min_vaddr, &file_offset_of_min_vaddr,
                                                  &symbols, &dex_file_offsets)) {
        PrintIndented(2, "file_path %s\n", file_path.c_str());
        PrintIndented(2, "file_type %s\n", DsoTypeToString(static_cast<DsoType>(file_type)));
        PrintIndented(2, "min_vaddr 0x%" PRIx64 "\n", min_vaddr);
        PrintIndented(2, "file_offset_of_min_vaddr 0x%" PRIx64 "\n", file_offset_of_min_vaddr);
        PrintIndented(2, "symbols:\n");
        for (const auto& symbol : symbols) {
          PrintIndented(3, "%s [0x%" PRIx64 "-0x%" PRIx64 "]\n", symbol.DemangledName(),
                        symbol.addr, symbol.addr + symbol.len);
        }
        if (file_type == static_cast<uint32_t>(DSO_DEX_FILE)) {
          PrintIndented(2, "dex_file_offsets:\n");
          for (uint64_t offset : dex_file_offsets) {
            PrintIndented(3, "0x%" PRIx64 "\n", offset);
          }
        }
      }
    } else if (feature == FEAT_META_INFO) {
      PrintIndented(1, "meta_info:\n");
      for (auto& pair : record_file_reader_->GetMetaInfoFeature()) {
        PrintIndented(2, "%s = %s\n", pair.first.c_str(), pair.second.c_str());
      }
    } else if (feature == FEAT_AUXTRACE) {
      PrintIndented(1, "file_offsets_of_auxtrace_records:\n");
      for (auto offset : record_file_reader_->ReadAuxTraceFeature()) {
        PrintIndented(2, "%" PRIu64 "\n", offset);
      }
    }
  }
  return true;
}

}  // namespace

void RegisterDumpRecordCommand() {
  RegisterCommand("dump", [] { return std::unique_ptr<Command>(new DumpRecordCommand); });
}
