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

#define USING_LOG_PREFIX SQL_ENG
#include "ob_expr_ai_rerank.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"
#include "lib/utility/utility.h"
#include "common/json_type/ob_json_common.h"
 
using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace
{
// Read any JSON number node as double without relying on the concrete node type.
int json_number_to_double(oceanbase::common::ObJsonNode *node, double &value)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (oceanbase::common::ObJsonNodeType::J_INT == node->json_type()) {
    value = static_cast<double>(node->get_int());
  } else if (oceanbase::common::ObJsonNodeType::J_UINT == node->json_type()) {
    value = static_cast<double>(node->get_uint());
  } else if (oceanbase::common::ObJsonNodeType::J_DOUBLE == node->json_type()
             || oceanbase::common::ObJsonNodeType::J_DECIMAL == node->json_type()) {
    value = node->get_double();
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

// Read any JSON number node as int64. Returns false when the node is absent or
// not a number, so callers can fall back to the response order.
bool json_int_value(oceanbase::common::ObJsonNode *node, int64_t &value)
{
  bool found = false;
  if (OB_NOT_NULL(node)) {
    if (oceanbase::common::ObJsonNodeType::J_INT == node->json_type()) {
      value = node->get_int();
      found = true;
    } else if (oceanbase::common::ObJsonNodeType::J_UINT == node->json_type()) {
      value = static_cast<int64_t>(node->get_uint());
      found = true;
    } else if (oceanbase::common::ObJsonNodeType::J_DOUBLE == node->json_type()
               || oceanbase::common::ObJsonNodeType::J_DECIMAL == node->json_type()) {
      value = static_cast<int64_t>(node->get_double());
      found = true;
    }
  }
  return found;
}

bool json_string_equal(oceanbase::common::ObJsonNode *lhs, oceanbase::common::ObJsonNode *rhs)
{
  bool equal = false;
  if (OB_NOT_NULL(lhs) && OB_NOT_NULL(rhs)
      && oceanbase::common::ObJsonNodeType::J_STRING == lhs->json_type()
      && oceanbase::common::ObJsonNodeType::J_STRING == rhs->json_type()) {
    equal = static_cast<oceanbase::common::ObJsonString *>(lhs)->get_str()
            == static_cast<oceanbase::common::ObJsonString *>(rhs)->get_str();
  }
  return equal;
}

// Rank key of one rerank result element. Elements that are not JSON objects, or
// that carry no numeric score, are unranked: they keep the provider order and
// are placed after the ranked elements instead of failing or crashing.
struct RerankElemKey
{
  bool ranked_;
  bool has_index_;
  double score_;
  int64_t index_;
};

void get_rerank_elem_key(oceanbase::common::ObJsonNode *node,
                         const oceanbase::common::ObString &score_key,
                         const oceanbase::common::ObString &index_key,
                         RerankElemKey &key)
{
  key.ranked_ = false;
  key.has_index_ = false;
  key.score_ = 0.0;
  key.index_ = 0;
  if (OB_NOT_NULL(node) && oceanbase::common::ObJsonNodeType::J_OBJECT == node->json_type()) {
    oceanbase::common::ObJsonObject *obj =
        static_cast<oceanbase::common::ObJsonObject *>(node);
    double score = 0.0;
    int64_t index = 0;
    if (OB_SUCCESS == json_number_to_double(obj->get_value(score_key), score)) {
      key.ranked_ = true;
      key.score_ = score;
      if (json_int_value(obj->get_value(index_key), index)) {
        key.has_index_ = true;
        key.index_ = index;
      }
    }
  }
}

// True when lhs has to be ranked strictly before rhs: higher score first, then
// smaller original document index. Unranked elements compare equal so they keep
// whatever order they arrived in.
bool rerank_elem_before(const RerankElemKey &lhs, const RerankElemKey &rhs)
{
  bool before = false;
  if (lhs.ranked_ != rhs.ranked_) {
    before = lhs.ranked_;
  } else if (!lhs.ranked_) {
    before = false;
  } else if (lhs.score_ != rhs.score_) {
    before = lhs.score_ > rhs.score_;
  } else if (lhs.has_index_ && rhs.has_index_ && lhs.index_ != rhs.index_) {
    before = lhs.index_ < rhs.index_;
  }
  return before;
}

// Stable selection sort of one batch by rank key. A single batch used to be
// returned in raw provider order while several batches were merged by score, so
// the same input produced different orders depending on batch_size. Every batch
// is ordered first, which makes both cases produce one global order.
int sort_rerank_batch_by_score(oceanbase::common::ObIAllocator &allocator,
                               oceanbase::common::ObJsonArray *batch_array,
                               const oceanbase::common::ObString &score_key,
                               const oceanbase::common::ObString &index_key,
                               oceanbase::common::ObJsonArray *&sorted_array)
{
  int ret = oceanbase::common::OB_SUCCESS;
  oceanbase::common::ObJsonArray *result_array = nullptr;
  if (OB_ISNULL(batch_array)) {
    ret = oceanbase::common::OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(oceanbase::common::ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    const int64_t count = batch_array->element_count();
    char *used = nullptr;
    if (count > 0 && OB_ISNULL(used = static_cast<char *>(allocator.alloc(count)))) {
      ret = oceanbase::common::OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (count > 0) {
        memset(used, 0, static_cast<size_t>(count));
      }
      for (int64_t picked = 0; OB_SUCC(ret) && picked < count; picked++) {
        int64_t best = -1;
        for (int64_t i = 0; i < count; i++) {
          if (0 == used[i]) {
            if (best < 0) {
              best = i;
            } else {
              RerankElemKey cur_key;
              RerankElemKey best_key;
              get_rerank_elem_key(batch_array->get_value(i), score_key, index_key, cur_key);
              get_rerank_elem_key(batch_array->get_value(best), score_key, index_key, best_key);
              if (rerank_elem_before(cur_key, best_key)) {
                best = i;
              }
            }
          }
        }
        if (best < 0) {
          ret = oceanbase::common::OB_ERR_UNEXPECTED;
        } else {
          used[best] = 1;
          if (OB_FAIL(result_array->append(batch_array->get_value(best)))) {
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_array = result_array;
  }
  return ret;
}
}  // namespace

namespace oceanbase 
{
namespace sql 
{

ObExprAIRerank::ObExprAIRerank(common::ObIAllocator &alloc)
: ObFuncExprOperator(alloc, 
                    T_FUN_SYS_AI_RERANK, 
                    N_AI_RERANK, 
                    MORE_THAN_ZERO,
                    NOT_VALID_FOR_GENERATED_COL, 
                    NOT_ROW_DIMENSION) 
{
}

ObExprAIRerank::~ObExprAIRerank() 
{
}

int ObExprAIRerank::calc_result_typeN(ObExprResType &type,
                                    ObExprResType *types_stack,
                                    int64_t param_num,
                                    common::ObExprTypeCtx &type_ctx) const {
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num < 3 || param_num > 4)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    if (!ob_is_string_tc(types_stack[MODEL_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model key must be string type");
    } else if (!ob_is_string_tc(types_stack[QUERY_IDX].get_type())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, query must be string type");
    } else {
      types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      types_stack[QUERY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    }
    ObObjType in_type = types_stack[DOCUMENTS_IDX].get_type();
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonExprHelper::is_valid_for_json(types_stack, DOCUMENTS_IDX, N_AI_RERANK))) {
    } else if (ob_is_string_type(in_type) && types_stack[DOCUMENTS_IDX].get_collation_type() != CS_TYPE_BINARY) {
      if (types_stack[DOCUMENTS_IDX].get_charset_type() != CHARSET_UTF8MB4) {
        types_stack[DOCUMENTS_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (param_num == 4) {
      ObObjType doc_key_type = types_stack[DOC_KEY_IDX].get_type();
      if (doc_key_type == ObNullType) {
      } else if (doc_key_type == ObJsonType) {
      } else if (ob_is_string_type(doc_key_type)) {
        types_stack[DOC_KEY_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
      } else {  
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, doc key must be string type");
      }
    }

    if (OB_SUCC(ret)) {
      type.set_json();
      type.set_length((ObAccuracy::DDL_DEFAULT_ACCURACY[ObJsonType]).get_length());
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_query = nullptr;
  ObDatum *arg_documents = nullptr;
  ObDatum *arg_doc_key = nullptr;
  if (OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_query, arg_documents, arg_doc_key))) {
  } else if (arg_model_id->is_null() || arg_query->is_null() || arg_documents->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, model id or query or documents is null");
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_RERANK));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString query = arg_query->get_string();
    ObArray<ObString> header_array;
    ObJsonArray *document_array = nullptr;
    ObJsonArray *result_array = nullptr;
    ObIJsonBase *j_base = nullptr;
    int64_t batch_size = 20; // max batch size for rerank
    bool is_null_result = false;
    // A SQL NULL doc_key means "no key was supplied", so the documents array is
    // read as plain strings. Treating the NULL datum as present made the string
    // form unusable and rejected it with "element is not object".
    const bool has_doc_key = OB_NOT_NULL(arg_doc_key) && !arg_doc_key->is_null();
    if (OB_FAIL(ObJsonExprHelper::get_json_doc(expr, ctx, temp_allocator, DOCUMENTS_IDX,
          j_base, is_null_result))) {
    } else if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (j_base->json_type() != ObJsonNodeType::J_ARRAY) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is not array");
    } else if (OB_ISNULL(document_array = static_cast<ObJsonArray *>(j_base))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (document_array->element_count() == 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array is empty");
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i++) {
        ObJsonNode *doc_node = document_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (has_doc_key && doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else if (!has_doc_key && doc_node->json_type() != ObJsonNodeType::J_STRING) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not string");
        }
      }
    }
    
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } 

    if (OB_FAIL(ret)) {
    } else if (has_doc_key) {
      ObString doc_key = arg_doc_key->get_string();
      if (OB_FAIL(eval_ai_rerank_with_doc_key(expr, ctx, temp_allocator, model_id,
                  query, document_array, doc_key, *info, *endpoint_info, res))) {
      }
    } else {
      if (OB_ISNULL(info)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(ObAIFuncUtils::check_info_type_rerank(info))) {
      } else if (OB_FAIL(ObAIFuncUtils::get_header(temp_allocator, *info, *endpoint_info, header_array))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(temp_allocator, result_array))) {
      }

      if (OB_SUCC(ret)) {
        ObString score_key(SCORE_KEY);
        ObString index_key(INDEX_KEY);
        int64_t end_idx = 0;
        ObJsonArray *compact_array = nullptr;
        ObJsonArray *batch_result_array = nullptr;
        ObJsonArray *sorted_batch_array = nullptr;
        ObJsonArray *batch_document_array = nullptr;
        for (int64_t i = 0; OB_SUCC(ret) && i < document_array->element_count(); i += batch_size) {
          end_idx = i + batch_size;
          if (end_idx > document_array->element_count()) {
            end_idx = document_array->element_count();
          }
          if (OB_FAIL(construct_batch_document_array(temp_allocator, document_array, i, end_idx, batch_document_array))) {
          } else if (OB_FAIL(inner_eval_ai_rerank(temp_allocator, *info, *endpoint_info, header_array, query, batch_document_array, batch_result_array))) {
          } else if (OB_FAIL(batch_result_add_base(temp_allocator, batch_result_array, i))) {
          } else if (OB_FAIL(sort_rerank_batch_by_score(temp_allocator, batch_result_array, score_key, index_key, sorted_batch_array))) {
          } else if (OB_FAIL(compact_json_array_by_key(temp_allocator, result_array, sorted_batch_array, score_key, compact_array))) {
          } else {
            result_array = compact_array;
          }
        }
      }
      if (OB_SUCC(ret)) {
        ObString raw_str;
        if (OB_FAIL(result_array->get_raw_binary(raw_str, &temp_allocator))) {
        } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::eval_ai_rerank_with_doc_key(const ObExpr &expr, ObEvalCtx &ctx, ObIAllocator &allocator, 
                                                ObString& model_id, ObString& query, ObJsonArray *document_array,
                                                ObString& doc_key, const ObAIFuncExprInfo &info,
                                                const share::ObAiModelEndpointInfo &endpoint_info, ObDatum &res)
{
  INIT_SUCC(ret);
  ObJsonArray *doc_array = nullptr;
  ObJsonArray *result_array = nullptr;
  ObJsonArray *sorted_document_array = nullptr;
  if (OB_FAIL(get_doc_array_from_documents_array_with_key(allocator, document_array, doc_key, doc_array))) {
  } else {
    ObAIFuncModel model(allocator, info, endpoint_info);
    if (OB_FAIL(model.call_rerank(query, doc_array, result_array))) {
    } else if (OB_FAIL(sort_document_array_by_model_result(allocator, document_array, result_array, doc_key, sorted_document_array))) {
    } 
  }
  if (OB_SUCC(ret)) {
    ObString raw_str;
    if (OB_FAIL(sorted_document_array->get_raw_binary(raw_str, &allocator))) {
    } else if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(expr, ctx, res, raw_str))) {
    }
  }
  return ret;
}

int ObExprAIRerank::get_doc_array_from_documents_array_with_key(ObIAllocator &allocator, ObJsonArray *document_object_array, ObString &doc_key, ObJsonArray *&doc_array)
{
  INIT_SUCC(ret);
  if (OB_ISNULL(document_object_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (doc_key.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObJsonArray *doc_array_obj = nullptr;
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, doc_array_obj))) {
    } else {
      int64_t count = document_object_array->element_count();
      ObJsonNode *doc_node = nullptr;
      ObJsonNode *doc_value_node = nullptr;
      ObJsonObject *doc_obj = nullptr;
      for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
        doc_node = document_object_array->get_value(i);
        if (OB_ISNULL(doc_node)) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is null");
        } else if (doc_node->json_type() != ObJsonNodeType::J_OBJECT) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, document_array element is not object");
        } else {
          doc_obj = static_cast<ObJsonObject *>(doc_node);
          doc_value_node = doc_obj->get_value(doc_key);
          if (OB_ISNULL(doc_value_node)) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is null");
          } else if (doc_value_node->json_type() != ObJsonNodeType::J_STRING) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR (OB_INVALID_ARGUMENT, "ai_rerank, the value for given doc key is not string");
          } else if (OB_FAIL(doc_array_obj->append(doc_value_node))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      doc_array = doc_array_obj;
    }
  }
  return ret;
}

int ObExprAIRerank::sort_document_array_by_model_result(ObIAllocator &allocator, ObJsonArray *document_array, ObJsonArray *model_result_array, const ObString &doc_key, ObJsonArray *&sorted_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *sorted_document_array_obj = nullptr;
  if (OB_ISNULL(document_array) || OB_ISNULL(model_result_array)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, sorted_document_array_obj))) {
  } else {
    const int64_t doc_count = document_array->element_count();
    const int64_t count = model_result_array->element_count();
    char *used_index = nullptr;
    if (doc_count > 0 && OB_ISNULL(used_index = static_cast<char *>(allocator.alloc(doc_count)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (doc_count > 0) {
      memset(used_index, 0, static_cast<size_t>(doc_count));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
      ObJsonNode *result_node = model_result_array->get_value(i);
      int64_t index = -1;
      ObJsonNode *score_node = nullptr;
      if (OB_ISNULL(result_node) || ObJsonNodeType::J_OBJECT != result_node->json_type()) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObJsonObject *result_obj = static_cast<ObJsonObject *>(result_node);
        score_node = result_obj->get_value(SCORE_KEY);
        double score_value = 0.0;
        if (OB_NOT_NULL(score_node) && OB_SUCCESS != json_number_to_double(score_node, score_value)) {
          // _model_score has to stay a number; a provider sending the score as
          // text used to leak that raw text into the result.
          ret = OB_INVALID_DATA;
          FORWARD_USER_ERROR(ret, "ai_rerank, relevance_score is not a number");
        } else if (!json_int_value(result_obj->get_value(INDEX_KEY), index)) {
          // A provider may echo the ranked documents instead of index/score
          // pairs; map such an element back through the text value that was
          // sent for this document key.
          index = -1;
          if (!doc_key.empty()) {
            ObJsonNode *text_node = result_obj->get_value(doc_key);
            for (int64_t k = 0; -1 == index && k < doc_count; k++) {
              ObJsonNode *origin_node = document_array->get_value(k);
              if (OB_NOT_NULL(origin_node) && ObJsonNodeType::J_OBJECT == origin_node->json_type()
                  && json_string_equal(static_cast<ObJsonObject *>(origin_node)->get_value(doc_key), text_node)) {
                index = k;
              }
            }
          }
          if (-1 == index && i < doc_count) {
            // Last resort: keep the response order.
            index = i;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (index < 0 || index >= doc_count) {
          ret = OB_ERR_UNEXPECTED;
        } else if (0 != used_index[index]) {
          // Answering one document twice would emit it twice and break the
          // count / original id mapping, so the response is rejected.
          ret = OB_INVALID_DATA;
          FORWARD_USER_ERROR(ret, "ai_rerank, duplicated index in model result");
        } else {
          used_index[index] = 1;
          ObJsonObject *origin_obj = static_cast<ObJsonObject *>(document_array->get_value(index));
          ObJsonObject *new_obj = nullptr;
          if (OB_ISNULL(origin_obj)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_ISNULL(new_obj = static_cast<ObJsonObject *>(origin_obj->clone(&allocator, true)))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_NOT_NULL(score_node) && OB_FAIL(new_obj->add(MODEL_SCORE_KEY, score_node))) {
          } else if (OB_FAIL(sorted_document_array_obj->append(new_obj))) {
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    sorted_document_array = sorted_document_array_obj;
  }
  return ret;
}

int ObExprAIRerank::construct_batch_document_array(ObIAllocator &allocator, ObJsonArray *document_array, int64_t start_idx, int64_t end_idx, ObJsonArray *&batch_document_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    for (int64_t i = start_idx; i < end_idx; i++) {
      if (OB_FAIL(result_array->append(document_array->get_value(i)))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    batch_document_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::inner_eval_ai_rerank(ObIAllocator &allocator, 
                                          const ObAIFuncExprInfo &info, 
                                          const share::ObAiModelEndpointInfo &endpoint_info,
                                          ObArray<ObString> &header_array, 
                                          ObString &query, 
                                          ObJsonArray *document_array, 
                                          ObJsonArray *&result_array)
{
  INIT_SUCC(ret);
  ObJsonArray *res = nullptr;
  ObJsonObject *config_json = nullptr;
  ObJsonObject *body = nullptr;
  ObJsonObject *http_response = nullptr;
  ObIJsonBase *response = nullptr;
  ObAIFuncClient ai_client;
  if (OB_FAIL(ObAIFuncUtils::get_rerank_body(allocator, info, endpoint_info, query, document_array, config_json, body))) {
  } else if (OB_FAIL(ai_client.send_post(allocator, endpoint_info.get_url(), header_array, body, http_response))) {
  } else if (OB_FAIL(ObAIFuncUtils::parse_rerank_output(allocator, endpoint_info, http_response, response))) {
  } else if (response->json_type() != ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(res = static_cast<ObJsonArray *>(response))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    result_array = res;
  }
  return ret;
}

int ObExprAIRerank::batch_result_add_base(ObIAllocator &allocator, ObJsonArray *array, int64_t start_idx)
{
  INIT_SUCC(ret);
  int64_t count = array->element_count();
  ObString index_key(INDEX_KEY);
  for (int64_t i = 0; OB_SUCC(ret) && i < count; i++) {
    ObJsonNode *node = array->get_value(i);
    if (OB_ISNULL(node)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (ObJsonNodeType::J_OBJECT != node->json_type()) {
      // The provider echoed the ranked documents as plain values; there is no
      // index field to shift and the array order already is the ranked order.
    } else {
      ObJsonObject *result_obj = static_cast<ObJsonObject *>(node);
      ObJsonNode *index_node = result_obj->get_value(index_key);
      int64_t index = 0;
      if (OB_ISNULL(index_node)) {
        // No index in the response: keep the provider order.
      } else if (!json_int_value(index_node, index)) {
        ret = OB_INVALID_DATA;
        FORWARD_USER_ERROR(ret, "ai_rerank, result index is not a number");
      } else if (index + start_idx < 0) {
        ret = OB_INVALID_DATA;
        FORWARD_USER_ERROR(ret, "ai_rerank, result index is out of range");
      } else {
        // Store the global position as a plain integer whatever encoding the
        // provider used (J_INT/J_UINT/J_DOUBLE); only the J_INT case used to be
        // shifted, so every other encoding kept a batch local index.
        ObJsonInt *base_index_node = nullptr;
        if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, index + start_idx, base_index_node))) {
        } else if (OB_FAIL(result_obj->add(index_key, base_index_node))) {
        }
      }
    }
  }
  return ret;
}

int ObExprAIRerank::compact_json_array_by_key(ObIAllocator &allocator, ObJsonArray *array1, ObJsonArray *array2, ObString &score_key, ObJsonArray *&compact_array)
{
  INIT_SUCC(ret);
  ObJsonArray *result_array = nullptr;
  if (OB_ISNULL(array1) || OB_ISNULL(array2)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_array(allocator, result_array))) {
  } else {
    const ObString index_key(INDEX_KEY);
    const int64_t count1 = array1->element_count();
    const int64_t count2 = array2->element_count();
    int64_t i = 0;
    int64_t j = 0;
    RerankElemKey key1;
    RerankElemKey key2;
    // Standard two pointer merge of the two already ordered arrays. Every
    // element of both arrays takes part, so a batch that answers fewer elements
    // than it was asked for (a provider truncating by top_n) can no longer hide
    // the scores of the other batch.
    while (i < count1 && j < count2 && OB_SUCC(ret)) {
      get_rerank_elem_key(array1->get_value(i), score_key, index_key, key1);
      get_rerank_elem_key(array2->get_value(j), score_key, index_key, key2);
      if (rerank_elem_before(key2, key1)) {
        if (OB_FAIL(result_array->append(array2->get_value(j)))) {
        } else {
          j++;
        }
      } else if (OB_FAIL(result_array->append(array1->get_value(i)))) {
      } else {
        // Equal keys keep the earlier batch in front: on a tie the later batch
        // used to jump ahead and reverse the original order.
        i++;
      }
    }
    if (OB_SUCC(ret)) {
      while (i < count1) {
        if (OB_FAIL(result_array->append(array1->get_value(i)))) {
        }
        i++;
      }
    }
    if (OB_SUCC(ret)) {
      while (j < count2) {
        if (OB_FAIL(result_array->append(array2->get_value(j)))) {
        }
        j++;
      }
    }
  }
  if (OB_SUCC(ret)) {
    compact_array = result_array;
  }
  return ret;
}

int ObExprAIRerank::construct_config_json(ObIAllocator &allocator, int64_t top_n, int64_t return_doc, ObJsonObject *&config_json)
{
  INIT_SUCC(ret);
  ObJsonObject *config_obj = nullptr;
  ObJsonInt *top_n_obj = nullptr;
  ObJsonBoolean *return_doc_obj = nullptr;
  bool return_doc_value = true;
  if (return_doc <= 0) {
    return_doc_value = false;
  }
  if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(allocator, config_obj))) {
  } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_boolean(allocator, return_doc_value, return_doc_obj))) {
  } else if (OB_FAIL(config_obj->add("return_documents", return_doc_obj))) {
  } else if (top_n > 0) {
    if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(allocator, top_n, top_n_obj))) {
    } else if (OB_FAIL(config_obj->add("top_n", top_n_obj))) {
    }
  }
  if (OB_SUCC(ret)) {
    config_json = config_obj;
  }
  return ret;
}

int ObExprAIRerank::cg_expr(ObExprCGCtx &expr_cg_ctx, 
                        const ObRawExpr &raw_expr,
                        ObExpr &rt_expr) const 
{
  int ret = OB_SUCCESS;
  // TODO: support schema version match in plan cache for ai func
  // const ObRawExpr *model_key = raw_expr.get_param_expr(0);
  // if (OB_NOT_NULL(model_key)
  //     && (model_key->is_static_scalar_const_expr() || model_key->is_const_expr())
  //     && model_key->get_expr_type() != T_OP_GET_USER_VAR &&
  //     OB_NOT_NULL(expr_cg_ctx.schema_guard_)) {
  //   ObIAllocator *allocator = expr_cg_ctx.allocator_;
  //   ObExecContext *exec_ctx = expr_cg_ctx.session_->get_cur_exec_ctx();
  //   bool got_data = false;
  //   ObObj const_data;
  //   ObAIFuncExprInfo *info = nullptr;
  //   if (OB_ISNULL(allocator)) {
  //     ret = OB_ERR_UNEXPECTED;
  //     LOG_WARN("allocator is null", K(ret));
  //   } else if (OB_FAIL(ObSQLUtils::calc_const_or_calculable_expr(exec_ctx,
  //                                                         model_key,
  //                                                         const_data,
  //                                                         got_data,
  //                                                         *allocator))) {
  //     LOG_WARN("failed to calc offset expr", K(ret));
  //   } else if (!got_data || const_data.is_null()) {
  //   } else {
  //     ObAIFuncExprInfo *info = OB_NEWx(ObAIFuncExprInfo, allocator, *allocator, T_FUN_SYS_AI_RERANK);
  //     if (OB_ISNULL(info)) {
  //       ret = OB_ALLOCATE_MEMORY_FAILED;
  //       LOG_WARN("failed to allocate memory for ai rerank info", K(ret));
  //     } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //       LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //     } else {
  //       rt_expr.extra_info_ = info;
  //     }
  //   }
  // }
  if (OB_SUCC(ret)) {
    rt_expr.eval_func_ = ObExprAIRerank::eval_ai_rerank;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
