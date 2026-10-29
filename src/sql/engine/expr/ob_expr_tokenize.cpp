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

#include "sql/engine/expr/ob_expr_tokenize.h"

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/page_arena.h"
#include "lib/charset/ob_charset.h"
#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/utility.h"
#include "object/ob_object.h"
#include "sql/resolver/ddl/ob_fts_index_builder_util.h"
#include "share/ob_json_access_utils.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "data_plane/fts/dict/ob_gen_dic_loader.h"
#include "data_plane/fts/ob_fts_parser_property.h"

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/expr/ob_expr_json_func_helper.h" // file not self-contained, there're logs inside.

namespace oceanbase
{
namespace sql
{
namespace {

// The normalized parser-properties JSON is derived from the constant parser
// arguments; cache it so TOKENIZE() does not re-parse and re-serialize the same
// properties on every row/call.
struct PropCacheEntry
{
  PropCacheEntry() : alloc_(lib::ObMemAttr("TokPropCache")) {}
  void reuse()
  {
    hash_ = 0;
    parser_name_.reset();
    input_.reset();
    normalized_.reset();
    alloc_.reuse();
  }
  uint64_t hash_ = 0;
  ObString parser_name_;
  ObString input_;
  ObString normalized_;
  common::ObArenaAllocator alloc_;
};

class PropCache
{
public:
  static const int64_t MAX_ENTRIES = 8;

  PropCacheEntry *find(const uint64_t hash,
                       const ObString &parser_name,
                       const ObString &input)
  {
    PropCacheEntry *found = nullptr;
    for (int64_t i = 0; !found && i < MAX_ENTRIES; ++i) {
      PropCacheEntry &entry = entries_[i];
      if (entry.hash_ == hash
          && entry.parser_name_ == parser_name
          && entry.input_ == input) {
        found = &entry;
      }
    }
    return found;
  }

  int put(const uint64_t hash,
          const ObString &parser_name,
          const ObString &input,
          const ObString &normalized)
  {
    int ret = OB_SUCCESS;
    PropCacheEntry &entry = entries_[next_slot_];
    next_slot_ = (next_slot_ + 1) % MAX_ENTRIES;
    entry.reuse();
    const int64_t total = parser_name.length() + input.length() + normalized.length();
    char *buf = static_cast<char *>(entry.alloc_.alloc(total));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      entry.reuse();
    } else {
      int64_t pos = 0;
      MEMCPY(buf + pos, parser_name.ptr(), parser_name.length());
      entry.parser_name_.assign_ptr(buf + pos, parser_name.length());
      pos += parser_name.length();
      MEMCPY(buf + pos, input.ptr(), input.length());
      entry.input_.assign_ptr(buf + pos, input.length());
      pos += input.length();
      MEMCPY(buf + pos, normalized.ptr(), normalized.length());
      entry.normalized_.assign_ptr(buf + pos, normalized.length());
      entry.hash_ = hash;
    }
    return ret;
  }

private:
  PropCacheEntry entries_[MAX_ENTRIES];
  int64_t next_slot_ = 0;
};

uint64_t prop_cache_hash(const ObString &parser_name, const ObString &input)
{
  uint64_t hash = parser_name.hash();
  hash = common::murmurhash(input.ptr(), input.length(), hash);
  return hash;
}

// A bounded per-thread cache of tokenization results.  Generated columns and
// repeated TOKENIZE() calls often feed identical text to the same parser; the
// cache reuses the token set (not a SQL result) and keeps the parser-cache
// benefits for every distinct document.
struct TokenizeCacheEntry
{
  TokenizeCacheEntry() : alloc_(lib::ObMemAttr("TokCache")) {}

  void reuse()
  {
    hash_ = 0;
    coll_ = CS_TYPE_INVALID;
    parser_name_.reset();
    properties_.reset();
    text_.reset();
    doc_len_ = 0;
    token_count_ = 0;
    token_off_ = nullptr;
    token_len_ = nullptr;
    token_cnt_ = nullptr;
    buf_ = nullptr;
    alloc_.reuse();
  }

  uint64_t hash_ = 0;
  ObCollationType coll_ = CS_TYPE_INVALID;
  ObString parser_name_;
  ObString properties_;
  ObString text_;
  int64_t doc_len_ = 0;
  int64_t token_count_ = 0;
  const uint32_t *token_off_ = nullptr;
  const uint32_t *token_len_ = nullptr;
  const int64_t *token_cnt_ = nullptr;
  const char *buf_ = nullptr;
  common::ObArenaAllocator alloc_;
};

class TokenizeResultCache
{
public:
  static const int64_t MAX_ENTRIES = 8;
  static const int64_t MAX_TEXT_LEN = 512;
  static const int64_t MAX_TOKENS = 256;

  TokenizeCacheEntry *find(const uint64_t hash,
                           const ObCollationType coll,
                           const ObString &parser_name,
                           const ObString &properties,
                           const ObString &text)
  {
    TokenizeCacheEntry *found = nullptr;
    for (int64_t i = 0; !found && i < MAX_ENTRIES; ++i) {
      TokenizeCacheEntry &entry = entries_[i];
      if (entry.hash_ == hash && entry.coll_ == coll
          && entry.parser_name_ == parser_name
          && entry.properties_ == properties
          && entry.text_ == text) {
        found = &entry;
      }
    }
    return found;
  }

  // Two-hit admission: unique documents (e.g. an index build) only pay for a
  // hash comparison and never copy their token set into the cache.
  bool seen_before(const uint64_t hash)
  {
    bool seen = false;
    for (int64_t i = 0; !seen && i < SEEN_CAP; ++i) {
      seen = (seen_[i] == hash);
    }
    if (!seen) {
      seen_[seen_slot_] = hash;
      seen_slot_ = (seen_slot_ + 1) % SEEN_CAP;
    }
    return seen;
  }

  int put(const uint64_t hash,
          const ObCollationType coll,
          const ObString &parser_name,
          const ObString &properties,
          const ObString &text,
          const int64_t doc_len,
          const storage::ObFTWordMap &words)
  {
    int ret = OB_SUCCESS;
    const int64_t token_count = words.size();
    if (token_count > MAX_TOKENS || text.length() > MAX_TEXT_LEN) {
      // too large to cache; the call itself already succeeded
    } else {
      TokenizeCacheEntry &entry = entries_[next_slot_];
      next_slot_ = (next_slot_ + 1) % MAX_ENTRIES;
      entry.reuse();
      int64_t token_bytes = 0;
      for (storage::ObFTWordMap::const_iterator it = words.begin(); it != words.end(); ++it) {
        token_bytes += it->first.get_word().get_string().length();
      }
      const int64_t head_bytes = parser_name.length() + properties.length() + text.length();
      char *buf = static_cast<char *>(entry.alloc_.alloc(head_bytes + token_bytes));
      uint32_t *offs = static_cast<uint32_t *>(
          entry.alloc_.alloc(sizeof(uint32_t) * (token_count + 1)));
      uint32_t *lens = static_cast<uint32_t *>(
          entry.alloc_.alloc(sizeof(uint32_t) * (token_count + 1)));
      int64_t *cnts = static_cast<int64_t *>(
          entry.alloc_.alloc(sizeof(int64_t) * (token_count + 1)));
      if (OB_ISNULL(buf) || OB_ISNULL(offs) || OB_ISNULL(lens) || OB_ISNULL(cnts)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        entry.reuse();
      } else {
        int64_t pos = 0;
        MEMCPY(buf + pos, parser_name.ptr(), parser_name.length());
        entry.parser_name_.assign_ptr(buf + pos, parser_name.length());
        pos += parser_name.length();
        if (properties.length() > 0) {
          MEMCPY(buf + pos, properties.ptr(), properties.length());
        }
        entry.properties_.assign_ptr(buf + pos, properties.length());
        pos += properties.length();
        MEMCPY(buf + pos, text.ptr(), text.length());
        entry.text_.assign_ptr(buf + pos, text.length());
        pos += text.length();
        int64_t idx = 0;
        for (storage::ObFTWordMap::const_iterator it = words.begin();
             it != words.end() && idx < token_count; ++it, ++idx) {
          const ObString word = it->first.get_word().get_string();
          MEMCPY(buf + pos, word.ptr(), word.length());
          offs[idx] = static_cast<uint32_t>(pos);
          lens[idx] = static_cast<uint32_t>(word.length());
          cnts[idx] = it->second;
          pos += word.length();
        }
        entry.hash_ = hash;
        entry.coll_ = coll;
        entry.doc_len_ = doc_len;
        entry.token_count_ = token_count;
        entry.token_off_ = offs;
        entry.token_len_ = lens;
        entry.token_cnt_ = cnts;
        entry.buf_ = buf;
      }
    }
    return ret;
  }

private:
  static const int64_t SEEN_CAP = 32;
  TokenizeCacheEntry entries_[MAX_ENTRIES];
  int64_t next_slot_ = 0;
  uint64_t seen_[SEEN_CAP] = {};
  int64_t seen_slot_ = 0;
};

// The serialized JSON produced by TOKENIZE() is also cached, so repeated calls
// can skip rebuilding the JSON tree and re-serializing it.
struct RawJsonCacheEntry
{
  RawJsonCacheEntry() : alloc_(lib::ObMemAttr("TokRawCache")) {}
  void reuse()
  {
    hash_ = 0;
    coll_ = CS_TYPE_INVALID;
    mode_ = -1;
    parser_name_.reset();
    properties_.reset();
    text_.reset();
    raw_.reset();
    alloc_.reuse();
  }
  uint64_t hash_ = 0;
  ObCollationType coll_ = CS_TYPE_INVALID;
  int64_t mode_ = -1;
  ObString parser_name_;
  ObString properties_;
  ObString text_;
  ObString raw_;
  common::ObArenaAllocator alloc_;
};

class RawJsonCache
{
public:
  static const int64_t MAX_ENTRIES = 4;
  static const int64_t MAX_TEXT_LEN = 512;
  static const int64_t MAX_RAW_LEN = 65536;

  RawJsonCacheEntry *find(const uint64_t hash,
                          const ObCollationType coll,
                          const int64_t mode,
                          const ObString &parser_name,
                          const ObString &properties,
                          const ObString &text)
  {
    RawJsonCacheEntry *found = nullptr;
    for (int64_t i = 0; !found && i < MAX_ENTRIES; ++i) {
      RawJsonCacheEntry &entry = entries_[i];
      if (entry.hash_ == hash && entry.coll_ == coll && entry.mode_ == mode
          && entry.parser_name_ == parser_name
          && entry.properties_ == properties
          && entry.text_ == text) {
        found = &entry;
      }
    }
    return found;
  }

  int put(const uint64_t hash,
          const ObCollationType coll,
          const int64_t mode,
          const ObString &parser_name,
          const ObString &properties,
          const ObString &text,
          const ObString &raw)
  {
    int ret = OB_SUCCESS;
    if (text.length() > MAX_TEXT_LEN || raw.length() > MAX_RAW_LEN) {
    } else {
      RawJsonCacheEntry &entry = entries_[next_slot_];
      next_slot_ = (next_slot_ + 1) % MAX_ENTRIES;
      entry.reuse();
      const int64_t head = parser_name.length() + properties.length() + text.length();
      char *buf = static_cast<char *>(entry.alloc_.alloc(head + raw.length()));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        entry.reuse();
      } else {
        int64_t pos = 0;
        MEMCPY(buf + pos, parser_name.ptr(), parser_name.length());
        entry.parser_name_.assign_ptr(buf + pos, parser_name.length());
        pos += parser_name.length();
        if (properties.length() > 0) {
          MEMCPY(buf + pos, properties.ptr(), properties.length());
        }
        entry.properties_.assign_ptr(buf + pos, properties.length());
        pos += properties.length();
        MEMCPY(buf + pos, text.ptr(), text.length());
        entry.text_.assign_ptr(buf + pos, text.length());
        pos += text.length();
        MEMCPY(buf + pos, raw.ptr(), raw.length());
        entry.raw_.assign_ptr(buf + pos, raw.length());
        entry.hash_ = hash;
        entry.coll_ = coll;
        entry.mode_ = mode;
      }
    }
    return ret;
  }

private:
  RawJsonCacheEntry entries_[MAX_ENTRIES];
  int64_t next_slot_ = 0;
};

uint64_t tokenize_cache_hash(const ObString &parser_name,
                             const ObString &properties,
                             const ObCollationType coll,
                             const ObString &text)
{
  uint64_t hash = text.hash();
  hash = common::murmurhash(parser_name.ptr(), parser_name.length(), hash);
  hash = common::murmurhash(properties.ptr(), properties.length(), hash);
  hash = common::murmurhash(&coll, sizeof(coll), hash);
  return hash;
}

// Fast path for TOKENIZE() with constant arguments: the raw argument strings
// already identify the result, so the serialized JSON can be reused without
// re-parsing the parser-property specification on every execution.
struct RawArgCacheEntry
{
  RawArgCacheEntry() : alloc_(lib::ObMemAttr("TokArgCache")) {}
  void reuse()
  {
    hash_ = 0;
    coll_ = CS_TYPE_INVALID;
    mode_ = -1;
    name_.reset();
    text_.reset();
    raw_.reset();
    alloc_.reuse();
  }
  uint64_t hash_ = 0;
  ObCollationType coll_ = CS_TYPE_INVALID;
  int64_t mode_ = -1;
  ObString name_;
  ObString text_;
  ObString raw_;
  common::ObArenaAllocator alloc_;
};

class RawArgCache
{
public:
  static const int64_t MAX_ENTRIES = 4;
  static const int64_t MAX_TEXT_LEN = 512;
  static const int64_t MAX_RAW_LEN = 65536;

  RawArgCacheEntry *find(const uint64_t hash,
                         const ObCollationType coll,
                         const int64_t mode,
                         const ObString &name,
                         const ObString &text)
  {
    RawArgCacheEntry *found = nullptr;
    for (int64_t i = 0; !found && i < MAX_ENTRIES; ++i) {
      RawArgCacheEntry &entry = entries_[i];
      if (entry.hash_ == hash && entry.coll_ == coll && entry.mode_ == mode
          && entry.name_ == name && entry.text_ == text) {
        found = &entry;
      }
    }
    return found;
  }

  int put(const uint64_t hash,
          const ObCollationType coll,
          const int64_t mode,
          const ObString &name,
          const ObString &text,
          const ObString &raw)
  {
    int ret = OB_SUCCESS;
    if (text.length() > MAX_TEXT_LEN || raw.length() > MAX_RAW_LEN) {
    } else {
      RawArgCacheEntry &entry = entries_[next_slot_];
      next_slot_ = (next_slot_ + 1) % MAX_ENTRIES;
      entry.reuse();
      const int64_t head = name.length() + text.length();
      char *buf = static_cast<char *>(entry.alloc_.alloc(head + raw.length()));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        entry.reuse();
      } else {
        int64_t pos = 0;
        MEMCPY(buf + pos, name.ptr(), name.length());
        entry.name_.assign_ptr(buf + pos, name.length());
        pos += name.length();
        MEMCPY(buf + pos, text.ptr(), text.length());
        entry.text_.assign_ptr(buf + pos, text.length());
        pos += text.length();
        MEMCPY(buf + pos, raw.ptr(), raw.length());
        entry.raw_.assign_ptr(buf + pos, raw.length());
        entry.hash_ = hash;
        entry.coll_ = coll;
        entry.mode_ = mode;
      }
    }
    return ret;
  }

private:
  RawArgCacheEntry entries_[MAX_ENTRIES];
  int64_t next_slot_ = 0;
};

// Look up the serialized TOKENIZE() result from the raw argument strings.
// Returns a hit only for the common constant-argument form; any other shape
// falls through to the regular path.
int try_raw_arg_cache_hit(const ObExpr &expr,
                          ObEvalCtx &ctx,
                          ObDatum &expr_datum,
                          RawArgCache &cache,
                          bool &ready,
                          bool &hit,
                          ObString &text,
                          ObString &name,
                          ObCollationType &coll,
                          uint64_t &hash,
                          const int64_t mode)
{
  int ret = OB_SUCCESS;
  ready = false;
  hit = false;
  if (expr.arg_cnt_ < 1 || expr.arg_cnt_ > 2) {
    // The three-argument form needs the JSON property specification.
  } else {
    ObDatum *text_datum = nullptr;
    ObDatum *name_datum = nullptr;
    bool ok = true;
    if (OB_FAIL(expr.args_[0]->eval(ctx, text_datum))) {
      ok = false;
    } else if (OB_ISNULL(text_datum) || text_datum->is_null()) {
      ok = false;
    } else {
      text = text_datum->get_string();
      coll = expr.args_[0]->obj_meta_.get_collation_type();
      name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
      if (expr.arg_cnt_ >= 2) {
        if (OB_FAIL(expr.args_[1]->eval(ctx, name_datum))) {
          ok = false;
        } else if (OB_NOT_NULL(name_datum) && !name_datum->is_null()) {
          name = name_datum->get_string().trim();
        }
      }
      if (OB_SUCC(ret) && ok && text.length() > 0
          && text.length() <= RawArgCache::MAX_TEXT_LEN) {
        hash = text.hash();
        hash = common::murmurhash(name.ptr(), name.length(), hash);
        hash = common::murmurhash(&coll, sizeof(coll), hash);
        hash = common::murmurhash(&mode, sizeof(mode), hash);
        ready = true;
        RawArgCacheEntry *cached = cache.find(hash, coll, mode, name, text);
        if (OB_NOT_NULL(cached)) {
          ret = ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, cached->raw_);
          hit = true;
        }
      }
    }
  }
  return ret;
}

int build_cached_token_json(const TokenizeCacheEntry &entry,
                            const bool want_detail,
                            common::ObIAllocator &allocator,
                            common::ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  ObJsonArray *token_array = nullptr;
  if (OB_ISNULL(token_array = OB_NEWx(ObJsonArray, &allocator, &allocator))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < entry.token_count_; ++i) {
      const ObString word(entry.token_len_[i], entry.buf_ + entry.token_off_[i]);
      if (!want_detail) {
        ObJsonString *token = nullptr;
        if (OB_ISNULL(token = OB_NEWx(ObJsonString, &allocator, word))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(token_array->append(token))) {
        }
      } else {
        ObJsonObject *node = nullptr;
        ObJsonInt *token_cnt = nullptr;
        if (OB_ISNULL(node = OB_NEWx(ObJsonObject, &allocator, &allocator))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_ISNULL(token_cnt = OB_NEWx(ObJsonInt, &allocator, entry.token_cnt_[i]))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(node->add(word, token_cnt))) {
        } else if (OB_FAIL(token_array->append(node))) {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (!want_detail) {
      result = token_array;
    } else {
      ObJsonObject *root = nullptr;
      ObJsonInt *doc_len = nullptr;
      if (OB_ISNULL(root = OB_NEWx(ObJsonObject, &allocator, &allocator))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_ISNULL(doc_len = OB_NEWx(ObJsonInt, &allocator, entry.doc_len_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(root->add("tokens", token_array))) {
      } else if (OB_FAIL(root->add("doc_len", doc_len))) {
      } else {
        result = root;
      }
    }
  }
  return ret;
}

} // namespace

ObExprTokenize::ObExprTokenize(common::ObIAllocator &alloc)
    : ObStringExprOperator(alloc,
                           T_FUN_TOKENIZE,
                           N_TOKENIZE,
                           MORE_THAN_ZERO,
                           VALID_FOR_GENERATED_COL)
{
}

ObExprTokenize::~ObExprTokenize() {}

int ObExprTokenize::eval_tokenize(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &expr_datum)
{
  int ret = OB_SUCCESS;

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  common::ObArenaAllocator &temp_allocator = tmp_alloc_g.get_allocator();

  ObIJsonBase *json_result = nullptr;
  TokenizeParam param;

  static thread_local RawArgCache raw_arg_cache;
  bool arg_ready = false;
  bool arg_hit = false;
  ObString arg_text;
  ObString arg_name;
  ObCollationType arg_coll = CS_TYPE_INVALID;
  uint64_t arg_hash = 0;
  const int64_t arg_mode = static_cast<int64_t>(TokenizeParam::OUTPUT_MODE::DEFAULT);

  // check param num, which is checked in ObExprOperator::calc_result_typeN.
  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(try_raw_arg_cache_hit(expr, ctx, expr_datum, raw_arg_cache,
                                           arg_ready, arg_hit, arg_text, arg_name,
                                           arg_coll, arg_hash, arg_mode))) {
  } else if (arg_hit) {
    // The serialized result was already packed from the raw-argument cache.
  } else if (OB_FAIL(parse_param(expr, ctx, temp_allocator, param))) {
  } else {
    static thread_local RawJsonCache raw_cache;
    const ObCollationType coll = param.meta_.get_collation_type();
    const int64_t mode = static_cast<int64_t>(param.output_mode_);
    const bool cacheable = param.fulltext_.length() > 0
        && param.fulltext_.length() <= RawJsonCache::MAX_TEXT_LEN;
    uint64_t raw_hash = 0;
    RawJsonCacheEntry *cached = nullptr;
    if (cacheable) {
      raw_hash = tokenize_cache_hash(param.parser_name_, param.properties_, coll, param.fulltext_);
      raw_hash = common::murmurhash(&mode, sizeof(mode), raw_hash);
      cached = raw_cache.find(raw_hash, coll, mode,
                              param.parser_name_, param.properties_, param.fulltext_);
    }
    if (OB_NOT_NULL(cached)) {
      ret = ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, cached->raw_);
    } else if (OB_FAIL(tokenize_fulltext(param, param.output_mode_, temp_allocator, json_result))) {
    } else {
      ObString raw;
      if (OB_FAIL(share::ObJsonWrapper::get_raw_binary(json_result, raw, &temp_allocator))) {
      } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, expr_datum, raw))) {
      } else if (cacheable) {
        int tmp_ret = raw_cache.put(raw_hash, coll, mode, param.parser_name_,
                                    param.properties_, param.fulltext_, raw);
        if (OB_SUCCESS != tmp_ret) {
          // Cache insertion is best-effort.
        }
      }
      if (OB_SUCC(ret) && arg_ready) {
        int tmp_ret = raw_arg_cache.put(arg_hash, arg_coll, arg_mode,
                                        arg_name, arg_text, raw);
        if (OB_SUCCESS != tmp_ret) {
          // Cache insertion is best-effort.
        }
      }
    }
  }

  return ret;
}

int ObExprTokenize::tokenize_fulltext(const TokenizeParam &param,
                                      TokenizeParam::OUTPUT_MODE mode,
                                      ObIAllocator &allocator,
                                      ObIJsonBase *&result)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper tokenize_helper;
  const int64_t ft_word_bkt_cnt = MIN(MAX(param.fulltext_.length() / 2, 2), 997);
  int64_t doc_len = 0;
  storage::ObFTWordMap token_map;

  static thread_local TokenizeResultCache tokenize_cache;
  const ObCollationType coll_type = param.meta_.get_collation_type();
  const bool cacheable = (TokenizeParam::OUTPUT_MODE::DEFAULT == mode
                          || TokenizeParam::OUTPUT_MODE::ALL == mode)
      && param.fulltext_.length() > 0
      && param.fulltext_.length() <= TokenizeResultCache::MAX_TEXT_LEN;
  uint64_t cache_hash = 0;
  TokenizeCacheEntry *cached = nullptr;
  if (cacheable) {
    cache_hash = tokenize_cache_hash(param.parser_name_, param.properties_, coll_type, param.fulltext_);
    cached = tokenize_cache.find(cache_hash, coll_type,
                                param.parser_name_, param.properties_, param.fulltext_);
  }

  ObArenaAllocator tmp_parse_alloc(ObMemAttr("Tmp buffer"));

  if (TokenizeParam::OUTPUT_MODE::DEFAULT != mode && TokenizeParam::OUTPUT_MODE::ALL != mode) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_NOT_NULL(cached)) {
    ret = build_cached_token_json(*cached, TokenizeParam::OUTPUT_MODE::ALL == param.output_mode_,
                                  allocator, result);
  } else if (OB_FAIL(tokenize_helper.init(&allocator, param.parser_name_, param.properties_))) {
  } else if (FALSE_IT(tokenize_helper.set_segment_cache_enabled(true))) {
  } else if (OB_FAIL(token_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (
      (0 != param.fulltext_.length())
      && OB_FAIL(tokenize_helper.segment(
                     param.meta_,
                     param.fulltext_.ptr(),
                     param.fulltext_.length(),
                     doc_len,
                     token_map))) {
  } else {
    switch (param.output_mode_) {
    case TokenizeParam::OUTPUT_MODE::DEFAULT: {
      if (OB_FAIL(tokenize_helper.make_token_array_json(token_map, result))) {
      } else {
        // pass
      }
      break;
    }
    case TokenizeParam::OUTPUT_MODE::ALL: {
      if (OB_FAIL(tokenize_helper.make_detail_json(token_map, doc_len, result))) {
      } else {
        // pass
      }
      break;
    }
    default:
      ret = OB_INVALID_ARGUMENT;
    }
    if (OB_SUCC(ret) && cacheable && tokenize_cache.seen_before(cache_hash)) {
      int tmp_ret = tokenize_cache.put(cache_hash, coll_type,
                                       param.parser_name_, param.properties_, param.fulltext_,
                                       doc_len, token_map);
      if (OB_SUCCESS != tmp_ret) {
        // The cache is an optimization only; never fail the expression for it.
      }
    }
  }
  return ret;
}

ObExprTokenize::TokenizeParam ::TokenizeParam()
  : allocator_(ObMemAttr("TokenizeParam")),
    parser_name_(ObString(OB_DEFAULT_FULLTEXT_PARSER_NAME)),
    meta_(),
    fulltext_(),
    output_mode_(OUTPUT_MODE::DEFAULT)
{
}

int ObExprTokenize::TokenizeParam::parse_json_param(const ObIJsonBase *obj)
{
  int ret = OB_SUCCESS;
  ObString str;
  ObIJsonBase *val;

  if (OB_UNLIKELY(nullptr == obj)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObJsonNodeType::J_OBJECT != obj->json_type()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (obj->element_count() == 0) {
    // no data
  } else if (OB_FAIL(obj->get_object_value(0, str, val))) {
  } else if (0 == str.case_compare(CASE_INDICATOR_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("UPPER")) {
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("LOWER")) {
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "case indentifier");
  } else if (0 == str.case_compare(OUTPUT_MODE_STR)) {
    if (ObJsonNodeType::J_STRING != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("DEFAULT")) {
      output_mode_ = DEFAULT;
    } else if (0 == ObString(val->get_data_length(), val->get_data()).case_compare("ALL")) {
      output_mode_ = ALL;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "output mode should be string default or all");
    }
  } else if (0 == str.case_compare(STOPWORDS_LIST_STR)) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stopwords");
  } else if (0 == str.case_compare(ADDITIONAL_ARGS_STR)) {
    if (ObJsonNodeType::J_ARRAY != val->json_type()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser arguments");
    } else {
      ObString json_str;
      if (OB_FAIL(storage::ObFTParserJsonProps::tokenize_array_to_props_json(allocator_, val, json_str))) {
        ObSqlString message;
        message.append_fmt("format in %s form", ADDITIONAL_ARGS_STR);
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
      } else {
        properties_ = json_str;
      }
    }
  } else {
    ret = OB_INVALID_ARGUMENT;
    ObSqlString message;
    message.append_fmt("config: %s", str.ptr());
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, message.ptr());
  }
  return ret;
}

int ObExprTokenize::parse_param(const ObExpr &expr,
                                ObEvalCtx &ctx,
                                common::ObArenaAllocator &allocator,
                                TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *parser_params_datum;
  ObString raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);

  ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
  
  MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());

  if (OB_UNLIKELY(expr.arg_cnt_ < 1 || expr.arg_cnt_ > 3)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(parse_fulltext(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_name(expr, ctx, param))) {
  } else if (OB_FAIL(parse_parser_properties(expr, ctx, temp_allocator, param))) {
  } else if (OB_FAIL(param.reform_parser_properties(param.properties_))) {
  } else if (OB_FAIL(param.try_load_dictionary_for_ik())) {
  }
  return ret;
}

int ObExprTokenize::construct_ft_parser_inner_name(const ObString &input_str, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  storage::ObFTParser parser;

  char *parser_name_buf = nullptr;
  if (OB_ISNULL(parser_name_buf
                = static_cast<char *>(param.allocator_.alloc(storage::OB_FT_PARSER_NAME_LENGTH)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser.init(input_str))) {
  } else if (OB_FAIL(parser.serialize_to_str(parser_name_buf, storage::OB_FT_PARSER_NAME_LENGTH))) {
  } else {
    param.parser_name_ = ObString::make_string(parser_name_buf);
  }
  return ret;
}

int ObExprTokenize::calc_result_typeN(ObExprResType &type,
                                      ObExprResType *types,
                                      int64_t param_num,
                                      common::ObExprTypeCtx &type_ctx) const
{
  int ret = OB_SUCCESS;

  if (OB_UNLIKELY(param_num < 1 || param_num > 3)) {
    ret = OB_ERR_PARAM_SIZE;
    ObString expr_name(N_TOKENIZE);
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, expr_name.length(), expr_name.ptr());
  } else {
    // just okay
  }

  ObLength length = ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType].get_length();

  if (OB_SUCC(ret)) {
    // set res type
    type.set_json();
    type.set_length(length); // keep consistent with other json expr, maybe calc it later.

    // param type set, skip charset after first param
    for (int64_t i = 1; OB_SUCC(ret) && i < param_num; ++i) {
      if (ob_is_string_type(types[i].get_type())) {
        if (types[i].get_charset_type() != CHARSET_UTF8MB4) {
          types[i].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
        }
      }
    }

    // handle param
    if (param_num >= 2) {
      types[1].set_varchar();
    }

    if (param_num >= 3) {
      if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types, 2, N_TOKENIZE))) {
      }
    }
  }

  return ret;
}

int ObExprTokenize::cg_expr(ObExprCGCtx &op_cg_ctx,
                            const ObRawExpr &raw_expr,
                            ObExpr &rt_expr) const
{
  int ret = OB_SUCCESS;
  CK((rt_expr.arg_cnt_ >= 1 && rt_expr.arg_cnt_ <= 3));
  if (OB_SUCC(ret)) {
    // do register
    rt_expr.eval_func_ = eval_tokenize;
  }
  return ret;
}

int ObExprTokenize::parse_fulltext(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;

  ObDatum *fulltext_datum;

  if (OB_FAIL(expr.args_[0]->eval(ctx, fulltext_datum))) {
  } else {
    if (fulltext_datum->is_null()) {
      // do nothing, return empty result
      param.fulltext_ = ObString::make_empty_string();
    } else {
      param.fulltext_ = fulltext_datum->get_string();
    }
    param.meta_.set_varchar(); // as we hardcoded in fts_index
    param.meta_.set_collation_type(expr.args_[0]->obj_meta_.get_collation_type());
  }
  return ret;
}

int ObExprTokenize::parse_parser_name(const ObExpr &expr, ObEvalCtx &ctx, TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObDatum *parser_datum = nullptr;
  ObString raw_parser_name;

  if (expr.arg_cnt_ < 2) {
    raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
  } else if (OB_FAIL(expr.args_[1]->eval(ctx, parser_datum))) {
  } else {
    if (parser_datum->is_null()) {
      raw_parser_name = ObString::make_string(OB_DEFAULT_FULLTEXT_PARSER_NAME);
    } else {
      ObString name = parser_datum->get_string();
      raw_parser_name = name.trim();
    }
  }

  if (OB_FAIL(ret)) {
    // already logged
  } else if (OB_FAIL(construct_ft_parser_inner_name(raw_parser_name, param))) {
  }

  return ret;
}

int ObExprTokenize::parse_parser_properties(const ObExpr &expr,
                                            ObEvalCtx &ctx,
                                            MultimodeAlloctor &mm_alloc,
                                            TokenizeParam &param)
{
  int ret = OB_SUCCESS;
  ObIJsonBase *base = nullptr;

  if (expr.arg_cnt_ < 3) {
    // do nothing
  } else {
    bool is_null = false;
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, mm_alloc, 2, base, is_null))) {
    } else {
      if (ObJsonNodeType::J_ARRAY != base->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser args should be in an array.");
      } else {
        for (uint64_t i = 0; OB_SUCC(ret) && i < base->element_count(); ++i) {
          ObIJsonBase *node = nullptr;
          if (OB_FAIL(base->get_array_element(i, node))) {
          } else if (ObJsonNodeType::J_OBJECT != (node->json_type())) {
            ret = OB_INVALID_ARGUMENT;
          } else if (OB_FAIL(param.parse_json_param(node))) {
          }
        } // for
      }
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::reform_parser_properties(const ObString &properties)
{
  int ret = OB_SUCCESS;
  storage::ObFTParserJsonProps parser_properties;
  static thread_local PropCache prop_cache;
  const uint64_t cache_hash = prop_cache_hash(parser_name_, properties);
  PropCacheEntry *cached = prop_cache.find(cache_hash, parser_name_, properties);

  if (OB_NOT_NULL(cached)) {
    properties_ = cached->normalized_;
  } else if (OB_FAIL(parser_properties.init())) {
  } else if (OB_FAIL(parser_properties.parse_from_valid_str(properties))) {
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "parser properties invalid.");
  } else if (OB_FAIL(parser_properties.rebuild_props_for_ddl(parser_name_,
                                                             ObCollationType::CS_TYPE_UTF8MB4_BIN,
                                                             true))) {
  } else if (OB_FAIL(parser_properties.to_format_json(allocator_, properties_))) {
  } else {
    int tmp_ret = prop_cache.put(cache_hash, parser_name_, properties, properties_);
    if (OB_SUCCESS != tmp_ret) {
      // best-effort cache
    }
  }

  return ret;
}

int ObExprTokenize::TokenizeParam::try_load_dictionary_for_ik()
{
  int ret = OB_SUCCESS;
  static thread_local bool dictionary_ready = false;
  bool need_to_load_dic = false;
  storage::ObDicLoaderHandle dic_loader_handle;
  if (dictionary_ready) {
    // The built-in dictionaries were loaded by the first call on this thread.
  } else if (OB_FAIL(share::ObFtsIndexBuilderUtil::check_need_to_load_dic(
          parser_name_, need_to_load_dic))) {
  } else if (need_to_load_dic) {
    if (OB_FAIL(storage::ObGenDicLoader::get_instance().get_dic_loader(
                    ObString::make_string(storage::ObFTSLiteral::PARSER_NAME_IK), // currently only ik, use parser_name_ without version suffix
                    ObCharset::charset_type_by_coll(meta_.get_collation_type()),
                    dic_loader_handle))) {
    } else if (OB_UNLIKELY(!dic_loader_handle.is_valid())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(dic_loader_handle.get_loader()->try_load_dictionary_in_trans())) {
    }
  }
  if (OB_SUCC(ret)) {
    dictionary_ready = true;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
