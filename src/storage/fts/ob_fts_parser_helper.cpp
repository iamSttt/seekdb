/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "object/ob_object.h"
#include "storage/fts/ob_fts_struct.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ob_fts_parser_helper.h"

#include "common/json_type/ob_json_tree.h"
#include "share/ob_force_print_log.h"
#include "storage/fts/dict/ob_ft_dict_hub.h"
#include "storage/fts/ob_beng_ft_parser.h"
#include "storage/fts/ob_ik_ft_parser.h"
#include "storage/fts/ob_ngram2_ft_parser.h"
#include "storage/fts/ob_ngram_ft_parser.h"
#include "data_plane/fts/ob_fts_parser_property.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_whitespace_ft_parser.h"
#include "lib/lock/ob_mutex.h"

namespace oceanbase
{
namespace storage
{

const char *ObFTParser::NAME_STR[ObFTParser::ParserType::FTP_MAX] = {
#define FT_PARSER_TYPE(ftp_type, parser_name) #parser_name,
  FTS_BUILD_IN_PARSER_LIST
#undef FT_PARSER_TYPE
};

bool ObFTParser::is_builtin() const
{
  bool is_builtin = false;
#define FT_PARSER_TYPE(ftp_type, parser_name) is_builtin = is_builtin || is_##parser_name();
  FTS_BUILD_IN_PARSER_LIST
#undef FT_PARSER_TYPE
  return is_builtin;
}

int ObFTParser::init(const common::ObString &parser_name)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(parser_name_.set_name(parser_name))) {
  } else if (!is_builtin()) {
    ret = OB_FUNCTION_NOT_DEFINED;
    LOG_USER_ERROR(OB_FUNCTION_NOT_DEFINED, parser_name.length(), parser_name.ptr());
  } else {
    parser_version_ = BUILTIN_VERSION;
  }
  return ret;
}

int ObFTParser::parse_from_str(const char *parser_name, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(parser_name)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(buf_len >= OB_FT_PARSER_NAME_LENGTH)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    char name[OB_FT_PARSER_NAME_LENGTH];
    char *saveptr = nullptr;
    char *token = nullptr;
    char *end_ptr = nullptr;
    MEMCPY(name, parser_name, buf_len);
    name[buf_len] = '\0';
    if (OB_ISNULL(token = STRTOK_R(name, ".", &saveptr))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(parser_name_.set_name(token))) {
    } else if (OB_ISNULL(token = STRTOK_R(nullptr, ".", &saveptr))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ob_strtoll(token, end_ptr, parser_version_))) {
    } else if (OB_NOT_NULL(token = STRTOK_R(nullptr, ".", &saveptr))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (!is_builtin()) {
      ret = OB_FUNCTION_NOT_DEFINED;
      LOG_USER_ERROR(OB_FUNCTION_NOT_DEFINED, static_cast<int>(buf_len), parser_name);
    } else if (OB_UNLIKELY(!is_valid())) {
      ret = OB_INVALID_ARGUMENT;
    }
  }
  return ret;
}

// The fulltext parser name consists of two parts: name and version, e.g. default_parser.1,
// separated by dot. This function is designed to serialize them into cstring.
int ObFTParser::serialize_to_str(char *buf, const int64_t buf_len)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  if (OB_ISNULL(buf) || OB_UNLIKELY(buf_len < OB_FT_PARSER_NAME_LENGTH)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(!is_valid())) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(common::databuff_printf(buf, buf_len, pos, "%.*s.%ld", parser_name_.len(), parser_name_.str(),
          parser_version_))) {
  }
  return ret;
}

int ObFTParser::get_desc(const ObIFTParserDesc *&parser_desc) const
{
  int ret = OB_SUCCESS;
  parser_desc = nullptr;
  if (OB_UNLIKELY(!is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_space()) {
    static const ObWhiteSpaceFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_ngram()) {
    static const ObNgramFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_beng()) {
    static const ObBasicEnglishFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_ik()) {
    static const ObIKFTParserDesc desc;
    parser_desc = &desc;
  } else if (is_ngram2()) {
    static const ObNgram2FTParserDesc desc;
    parser_desc = &desc;
  } else {
    ret = OB_FUNCTION_NOT_DEFINED;
  }
  return ret;
}
////////////////////////////////////////////////////////////////////////////////
// ObFTParseData

static ObFTParseData *g_ftparse_data = nullptr;
static constexpr const char *FTPARSE_DATA_MEMORY_LABEL = "FtParse";

int ObFTParseData::init_global()
{
  int ret = OB_SUCCESS;
  if (OB_NOT_NULL(g_ftparse_data)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(g_ftparse_data = OB_NEW(ObFTParseData, FTPARSE_DATA_MEMORY_LABEL))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(g_ftparse_data->init())) {
  }
  return ret;
}

void ObFTParseData::deinit_global()
{
  if (OB_NOT_NULL(g_ftparse_data)) {
    OB_DELETE(ObFTParseData, FTPARSE_DATA_MEMORY_LABEL, g_ftparse_data);
    g_ftparse_data = nullptr;
  }
}

ObFTParseData &ObFTParseData::instance()
{
  return *g_ftparse_data;
}

ObFTParseData::~ObFTParseData()
{
  destroy();
}

int ObFTParseData::init()
{
  int ret = OB_SUCCESS;
  lib::ObMemAttr mem_attr;
  mem_attr.label_ = FTPARSE_DATA_MEMORY_LABEL;

  if (OB_FAIL(handler_allocator_.init(lib::ObMallocAllocator::get_instance(),
                                      OB_MALLOC_NORMAL_BLOCK_SIZE,
                                      mem_attr))) {
  } else if (OB_FAIL(init_and_set_stopword_list())) {
  } else if (OB_FAIL(init_dict_hub())) {
  } else {
    is_inited_ = true;
    FLOG_INFO("succeeded to initialize fulltext parser data", KP(this));
  }

  if (OB_UNLIKELY(!is_inited_)) {
    destroy();
  }
  return ret;
}

int ObFTParseData::init_and_set_stopword_list()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(stop_word_checker_ = OB_NEWx(ObStopWordChecker, &handler_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(stop_word_checker_->init())) {
  }

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObStopWordChecker, &handler_allocator_, stop_word_checker_);
    stop_word_checker_ = nullptr;
  }

  return ret;
}

int ObFTParseData::init_dict_hub()
{
  // make dict
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dict_hub_ = OB_NEWx(ObFTDictHub, &handler_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(dict_hub_->init())) {
  }
  return ret;
}

void ObFTParseData::destroy()
{
  if (OB_NOT_NULL(stop_word_checker_)) {
    stop_word_checker_->destroy();
    OB_DELETEx(ObStopWordChecker, &handler_allocator_, stop_word_checker_);
    stop_word_checker_ = nullptr;
  }

  if (!OB_ISNULL(dict_hub_)) {
    dict_hub_->destroy();
    dict_hub_->~ObFTDictHub();
    handler_allocator_.free(dict_hub_);
    dict_hub_ = nullptr;
  }

  handler_allocator_.reset();
  is_inited_ = false;
}

int ObFTParseData::get_dict_hub(ObFTDictHub *&hub)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(dict_hub_)) {
    ret = OB_NOT_INIT;
  } else {
    hub = dict_hub_;
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// ObFTParseHelper
int ObFTParseHelper::segment(
    const ObFTParserProperty &property,
    const int64_t parser_version,
    const ObIFTParserDesc *parser_desc,
    const ObCharsetInfo *cs,
    const char *ft,
    const int64_t ft_len,
    common::ObIAllocator &allocator,
    ObAddWord &add_word)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(parser_version < 0 || nullptr == parser_desc || nullptr == cs || nullptr == ft || 0 >= ft_len)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObFTParserParam param;
    ObITokenIterator *iter = nullptr;
    param.allocator_ = &allocator;
    param.cs_ = cs;
    param.fulltext_ = ft;
    param.ft_length_ = ft_len;
    param.parser_version_ = parser_version;
    param.ngram_token_size_ = property.ngram_token_size_;
    param.ik_param_.mode_
        = (property.ik_mode_smart_ ? ObFTIKParam::Mode::SMART : ObFTIKParam::Mode::MAX_WORD);
    param.min_ngram_size_ = property.min_ngram_token_size_;
    param.max_ngram_size_ = property.max_ngram_token_size_;

    if (OB_FAIL(parser_desc->segment(&param, iter))) {
    } else if (OB_ISNULL(iter)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const char *word = nullptr;
      int64_t word_len = 0;
      int64_t char_cnt = 0;
      int64_t word_freq = 0;
      while (OB_SUCC(ret)) {
        if (OB_FAIL(iter->get_next_token(word, word_len, char_cnt, word_freq))) {
          if (OB_ITER_END != ret) {
          }
        } else if (OB_FAIL(add_word.process_word(word, word_len, char_cnt, word_freq))) {
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
    if (OB_NOT_NULL(iter)) {
      parser_desc->free_token_iter(&param, iter);
      iter = nullptr;
    }
  }
  return ret;
}

ObFTParseHelper::ObFTParseHelper()
    : allocator_(nullptr),
    parser_desc_(nullptr),
    parser_name_(),
    add_word_flag_(),
    parser_property_(),
    is_inited_(false),
    segment_cache_enabled_(false)
{
}

ObFTParseHelper::~ObFTParseHelper()
{
  reset();
}

int ObFTParseHelper::init(
    common::ObIAllocator *allocator,
    const common::ObString &parser_name,
    const common::ObString &parser_properties)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(allocator) || OB_UNLIKELY(parser_name.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parser_name_.parse_from_str(parser_name.ptr(), parser_name.length()))) {
  } else if (OB_FAIL(parser_property_.parse_for_parser_helper(parser_name_, parser_properties))) {
  } else if (OB_FAIL(parser_name_.get_desc(parser_desc_))) {
  } else if (OB_ISNULL(parser_desc_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(set_add_word_flag(*parser_desc_))) {
  } else {
    allocator_ = allocator;
    is_inited_ = true;
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

namespace {

// Process-wide cache of per-document token sets.  An FTS index build feeds the
// same word_segment text to both the postings aux table and the doc_word aux
// table (and TOKENIZE() workloads repeat text), so the second consumer can reuse
// the first consumer's token set.  Entries are immutable and never freed, which
// keeps the ObFTWord keys valid for concurrent readers; insertion simply stops
// at the capacity bound.
struct FTSegmentCacheEntry
{
  uint64_t hash_ = 0;
  ObCollationType coll_ = CS_TYPE_INVALID;
  int64_t parser_version_ = -1;
  ObString parser_name_;
  ObFTParserProperty property_;
  ObString text_;
  int64_t doc_len_ = 0;
  int64_t token_count_ = 0;
  const uint32_t *token_off_ = nullptr;
  const uint32_t *token_len_ = nullptr;
  const int64_t *token_cnt_ = nullptr;
  const char *buf_ = nullptr;
};

class FTSegmentCache
{
public:
  // Only TOKENIZE() enables this cache; a modest bound keeps the process-wide
  // arena small even for workloads with many distinct inputs.
  static const int64_t MAX_ENTRIES = 8192;
  static const int64_t SLOT_CNT = 16384; // power of two, >= 2 * MAX_ENTRIES
  static const int64_t MAX_TEXT_LEN = 2048;
  static const int64_t MAX_TOKENS = 512;

  FTSegmentCache() : slots_(nullptr), count_(0), arena_(lib::ObMemAttr("FTSegCache")) {}
  ~FTSegmentCache() = default;

  int get(const uint64_t hash,
          const ObCollationType coll,
          const int64_t parser_version,
          const ObFTParserName &parser_name,
          const ObFTParserProperty &property,
          const ObString &text,
          FTSegmentCacheEntry *&entry)
  {
    int ret = OB_SUCCESS;
    entry = nullptr;
    lib::ObMutexGuard guard(lock_);
    if (OB_NOT_NULL(slots_)) {
      const ObString name(parser_name.len(), parser_name.str());
      for (int64_t probe = 0; probe < SLOT_CNT; ++probe) {
        FTSegmentCacheEntry *candidate = slots_[(hash + probe) & (SLOT_CNT - 1)];
        if (OB_ISNULL(candidate)) {
          break;
        } else if (candidate->hash_ == hash
                   && candidate->coll_ == coll
                   && candidate->parser_version_ == parser_version
                   && candidate->parser_name_ == name
                   && candidate->property_.is_equal(property)
                   && candidate->text_ == text) {
          entry = candidate;
          break;
        }
      }
    }
    return ret;
  }

  int put(const uint64_t hash,
          const ObCollationType coll,
          const int64_t parser_version,
          const ObFTParserName &parser_name,
          const ObFTParserProperty &property,
          const ObString &text,
          const int64_t doc_len,
          const ObFTWordMap &words)
  {
    int ret = OB_SUCCESS;
    const int64_t token_count = words.size();
    if (token_count > MAX_TOKENS || text.length() > MAX_TEXT_LEN) {
      // not cacheable
    } else {
      lib::ObMutexGuard guard(lock_);
      if (OB_FAIL(ensure_slots())) {
      } else if (count_ >= MAX_ENTRIES) {
        // cache full: keep existing entries and stop inserting
      } else {
        const ObString name(parser_name.len(), parser_name.str());
        int64_t slot = -1;
        for (int64_t probe = 0; probe < SLOT_CNT; ++probe) {
          const int64_t idx = (hash + probe) & (SLOT_CNT - 1);
          FTSegmentCacheEntry *candidate = slots_[idx];
          if (OB_ISNULL(candidate)) {
            slot = idx;
            break;
          } else if (candidate->hash_ == hash
                     && candidate->coll_ == coll
                     && candidate->parser_version_ == parser_version
                     && candidate->parser_name_ == name
                     && candidate->property_.is_equal(property)
                     && candidate->text_ == text) {
            break; // another thread inserted it first
          }
        }
        if (slot >= 0) {
          int64_t token_bytes = 0;
          for (ObFTWordMap::const_iterator it = words.begin(); it != words.end(); ++it) {
            token_bytes += it->first.get_word().get_string().length();
          }
          const int64_t prop_bytes = property.stopword_table_.length()
              + property.dict_table_.length() + property.quantifier_table_.length();
          const int64_t head_bytes = parser_name.len() + text.length() + prop_bytes;
          char *buf = static_cast<char *>(arena_.alloc(head_bytes + token_bytes));
          uint32_t *offs = static_cast<uint32_t *>(
              arena_.alloc(sizeof(uint32_t) * (token_count + 1)));
          uint32_t *lens = static_cast<uint32_t *>(
              arena_.alloc(sizeof(uint32_t) * (token_count + 1)));
          int64_t *cnts = static_cast<int64_t *>(
              arena_.alloc(sizeof(int64_t) * (token_count + 1)));
          if (OB_ISNULL(buf) || OB_ISNULL(offs) || OB_ISNULL(lens) || OB_ISNULL(cnts)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            int64_t pos = 0;
            MEMCPY(buf + pos, parser_name.str(), parser_name.len());
            const ObString stored_name(parser_name.len(), buf + pos);
            pos += parser_name.len();
            MEMCPY(buf + pos, text.ptr(), text.length());
            const ObString stored_text(text.length(), buf + pos);
            pos += text.length();
            ObFTParserProperty stored_prop;
            stored_prop.min_token_size_ = property.min_token_size_;
            stored_prop.max_token_size_ = property.max_token_size_;
            stored_prop.ngram_token_size_ = property.ngram_token_size_;
            stored_prop.ik_mode_smart_ = property.ik_mode_smart_;
            stored_prop.min_ngram_token_size_ = property.min_ngram_token_size_;
            stored_prop.max_ngram_token_size_ = property.max_ngram_token_size_;
            stored_prop.stopword_table_ = copy_cache_string(property.stopword_table_, buf, pos);
            stored_prop.dict_table_ = copy_cache_string(property.dict_table_, buf, pos);
            stored_prop.quantifier_table_ = copy_cache_string(property.quantifier_table_, buf, pos);
            int64_t idx = 0;
            for (ObFTWordMap::const_iterator it = words.begin();
                 it != words.end() && idx < token_count; ++it, ++idx) {
              const ObString word = it->first.get_word().get_string();
              MEMCPY(buf + pos, word.ptr(), word.length());
              offs[idx] = static_cast<uint32_t>(pos);
              lens[idx] = static_cast<uint32_t>(word.length());
              cnts[idx] = it->second;
              pos += word.length();
            }
            void *entry_buf = arena_.alloc(sizeof(FTSegmentCacheEntry));
            if (OB_ISNULL(entry_buf)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else {
              FTSegmentCacheEntry *entry = new (entry_buf) FTSegmentCacheEntry();
              entry->hash_ = hash;
              entry->coll_ = coll;
              entry->parser_version_ = parser_version;
              entry->parser_name_ = stored_name;
              entry->property_ = stored_prop;
              entry->text_ = stored_text;
              entry->doc_len_ = doc_len;
              entry->token_count_ = token_count;
              entry->token_off_ = offs;
              entry->token_len_ = lens;
              entry->token_cnt_ = cnts;
              entry->buf_ = buf;
              slots_[slot] = entry;
              ++count_;
            }
          }
        }
      }
    }
    return ret;
  }

private:
  static ObString copy_cache_string(const ObString &src, char *buf, int64_t &pos)
  {
    ObString stored;
    if (src.length() > 0) {
      MEMCPY(buf + pos, src.ptr(), src.length());
      stored.assign_ptr(buf + pos, src.length());
      pos += src.length();
    }
    return stored;
  }

  int ensure_slots()
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(slots_)) {
      const int64_t bytes = sizeof(FTSegmentCacheEntry *) * SLOT_CNT;
      void *buf = arena_.alloc(bytes);
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMSET(buf, 0, bytes);
        slots_ = static_cast<FTSegmentCacheEntry **>(buf);
      }
    }
    return ret;
  }

private:
  FTSegmentCacheEntry **slots_;
  int64_t count_;
  common::ObArenaAllocator arena_;
  lib::ObMutex lock_;
};

FTSegmentCache &get_fts_segment_cache()
{
  static FTSegmentCache cache;
  return cache;
}

uint64_t fts_segment_cache_hash(const ObFTParserName &parser_name,
                                const int64_t parser_version,
                                const ObFTParserProperty &property,
                                const ObCollationType coll,
                                const ObString &text)
{
  uint64_t hash = text.hash();
  hash = common::murmurhash(parser_name.str(), static_cast<int32_t>(parser_name.len()), hash);
  hash = common::murmurhash(&parser_version, sizeof(parser_version), hash);
  hash = common::murmurhash(&coll, sizeof(coll), hash);
  hash = common::murmurhash(&property.min_token_size_, sizeof(int64_t), hash);
  hash = common::murmurhash(&property.max_token_size_, sizeof(int64_t), hash);
  hash = common::murmurhash(&property.ngram_token_size_, sizeof(int64_t), hash);
  hash = common::murmurhash(&property.ik_mode_smart_, sizeof(bool), hash);
  hash = common::murmurhash(&property.min_ngram_token_size_, sizeof(int64_t), hash);
  hash = common::murmurhash(&property.max_ngram_token_size_, sizeof(int64_t), hash);
  hash = common::murmurhash(property.stopword_table_.ptr(), property.stopword_table_.length(), hash);
  hash = common::murmurhash(property.dict_table_.ptr(), property.dict_table_.length(), hash);
  hash = common::murmurhash(property.quantifier_table_.ptr(), property.quantifier_table_.length(), hash);
  return hash;
}

} // namespace

void ObFTParseHelper::reset()
{
  parser_desc_ = nullptr;
  allocator_ = nullptr;
  add_word_flag_.clear();
  is_inited_ = false;
}

int ObFTParseHelper::segment(
    const ObObjMeta &meta,
    const char *fulltext,
    const int64_t fulltext_len,
    int64_t &doc_length,
    ObFTWordMap &words) const
{
  int ret = OB_SUCCESS;
  const ObCharsetInfo *cs = nullptr;
  ObCollationType type = meta.get_collation_type();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(allocator_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(CS_TYPE_INVALID == type || type >= CS_TYPE_PINYIN_BEGIN_MARK)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(cs = common::ObCharset::get_charset(type))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObString fulltext_str(fulltext_len, fulltext);
    const ObFTParserName &cache_parser_name = parser_name_.get_parser_name();
    const int64_t parser_version = parser_name_.get_parser_version();
    const bool cacheable = segment_cache_enabled_
        && fulltext_len > 0
        && fulltext_len <= FTSegmentCache::MAX_TEXT_LEN;
    uint64_t cache_hash = 0;
    FTSegmentCacheEntry *cached = nullptr;
    if (cacheable) {
      cache_hash = fts_segment_cache_hash(cache_parser_name, parser_version,
                                          parser_property_, type, fulltext_str);
      if (OB_FAIL(get_fts_segment_cache().get(cache_hash, type, parser_version,
                                             cache_parser_name, parser_property_,
                                             fulltext_str, cached))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(cached)) {
      words.reuse();
      for (int64_t i = 0; OB_SUCC(ret) && i < cached->token_count_; ++i) {
        const ObString word(cached->token_len_[i], cached->buf_ + cached->token_off_[i]);
        if (OB_FAIL(words.set_refactored(ObFTWord(word.length(), word.ptr(), meta),
                                         cached->token_cnt_[i], 1 /*overwrite*/))) {
        }
      }
      if (OB_SUCC(ret)) {
        doc_length = cached->doc_len_;
      }
    } else {
      words.reuse();
      ObAddWord add_word(parser_property_, meta, add_word_flag_, *allocator_, words);
      if (OB_FAIL(segment(
                      parser_property_,
                      parser_version,
                      parser_desc_,
                      cs,
                      fulltext,
                      fulltext_len,
                      *allocator_,
                      add_word))) {
      } else {
        doc_length = add_word.get_add_word_count();
        if (cacheable) {
          int tmp_ret = get_fts_segment_cache().put(cache_hash, type, parser_version,
                                                    cache_parser_name, parser_property_,
                                                    fulltext_str, doc_length, words);
          if (OB_SUCCESS != tmp_ret) {
            // Cache insertion is best-effort; never fail the tokenization.
          }
        }
      }
    }
  }
  LOG_DEBUG("ft parse segment", K(ret), K(type), K(add_word_flag_), K(parser_name_),
      K(ObString(fulltext_len, fulltext)), K(words.size()));
  return ret;
}

int ObFTParseHelper::check_is_the_same(
    const common::ObString &parser_name_str,
    const common::ObString &parser_properties,
    bool &is_same) const
{
  int ret = OB_SUCCESS;
  is_same = false;
  if (is_inited_) {
    storage::ObFTParser parser_name;
    ObFTParserProperty parser_property;
    if (OB_UNLIKELY(parser_name_str.empty())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(parser_name.parse_from_str(parser_name_str.ptr(), parser_name_str.length()))) {
    } else if (OB_FAIL(parser_property.parse_for_parser_helper(parser_name, parser_properties))) {
    } else if (parser_name == parser_name_ && parser_property.is_equal(parser_property_)) {
      is_same = true;
    }
  }
  return ret;
}

int ObFTParseHelper::make_detail_json(
    const ObFTWordMap &words,
    const int64_t doc_length,
    common::ObIJsonBase *&json_root)
{
 int ret = OB_SUCCESS;

 ObJsonObject *root_obj = nullptr;

 ObJsonInt *cnt = nullptr;

 ObJsonArray *token_array = nullptr;

 if (OB_ISNULL(root_obj = OB_NEWx(ObJsonObject, allocator_, allocator_))) {
   ret = OB_ALLOCATE_MEMORY_FAILED;
 } else if (OB_ISNULL(cnt = OB_NEWx(ObJsonInt, allocator_, doc_length))) {
   ret = OB_ALLOCATE_MEMORY_FAILED;
 } else if (OB_ISNULL(token_array = OB_NEWx(ObJsonArray, allocator_, allocator_))) {
   ret = OB_ALLOCATE_MEMORY_FAILED;
 } else {
   for (ObFTWordMap::const_iterator it = words.begin(); OB_SUCC(ret) && it != words.end(); ++it) {
     ObString key = it->first.get_word().get_string();
     ObJsonObject *node = nullptr;
     ObJsonInt *token_cnt_node = nullptr;
     if (OB_ISNULL(node = OB_NEWx(ObJsonObject, allocator_, allocator_))) {
       ret = OB_ALLOCATE_MEMORY_FAILED;
     } else if (OB_ISNULL(token_cnt_node = OB_NEWx(ObJsonInt, allocator_, it->second))) {
       ret = OB_ALLOCATE_MEMORY_FAILED;
     } else if (OB_FAIL(node->add(key, token_cnt_node))) {
     } else if (OB_FAIL(token_array->append(node))) {
     } else {
       // pass
     }

     if (OB_FAIL(ret)) {
       OB_DELETEx(ObJsonObject, allocator_, node);
       OB_DELETEx(ObJsonInt, allocator_, token_cnt_node);
     }
   } // for

   if (OB_SUCC(ret)) {
     if (OB_FAIL(root_obj->add(ENTRY_NAME_TOKENS, token_array))) {
     } else if (OB_FAIL(root_obj->add(ENTRY_NAME_DOC_LEN, cnt))) {
     }
   }
 }
  if (OB_SUCC(ret)) {
    json_root = root_obj;
  } else {
    OB_DELETEx(ObJsonObject, allocator_, root_obj);
    OB_DELETEx(ObJsonInt, allocator_, cnt);
    OB_DELETEx(ObJsonArray, allocator_, token_array);
  }

  return ret;
}

int ObFTParseHelper::make_token_array_json(
    const ObFTWordMap &words,
    common::ObIJsonBase *&json_root)
{
  int ret = OB_SUCCESS;
  ObJsonArray *token_array = nullptr;
  if (OB_UNLIKELY(OB_ISNULL(token_array = OB_NEWx(ObJsonArray, allocator_, allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (ObFTWordMap::const_iterator it = words.begin(); OB_SUCC(ret) && it != words.end(); ++it) {
      ObString key = it->first.get_word().get_string();
      ObJsonString *token = nullptr;
      if (OB_UNLIKELY(OB_ISNULL(token = OB_NEWx(ObJsonString, allocator_, key)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (OB_FAIL(token_array->append(token))) {
          OB_DELETEx(ObJsonString, allocator_, token);
        } else {
        }
      }
    } // for
  }
  if (OB_SUCC(ret)) {
    json_root = token_array;
  } else {
    OB_DELETEx(ObJsonArray, allocator_, token_array);
  }
  return ret;
}

int ObFTParseHelper::set_add_word_flag(const ObIFTParserDesc &ftparser_desc)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ftparser_desc.get_add_word_flag(add_word_flag_))) {
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
