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
#include "ob_expr_ai_embed.h"
#include "query/ai/ob_ai_endpoint_resolver.h"
#include "share/rc/ob_server_runtime.h"

using namespace oceanbase::common;
using namespace oceanbase::sql;

namespace oceanbase 
{
namespace sql 
{
ObExprAIEmbed::ObExprAIEmbed(common::ObIAllocator &alloc)
    : ObFuncExprOperator(alloc, 
                        T_FUN_SYS_AI_EMBED, 
                        N_AI_EMBED, 
                        MORE_THAN_ZERO,
                        NOT_VALID_FOR_GENERATED_COL, 
                        NOT_ROW_DIMENSION) 
{
}

ObExprAIEmbed::~ObExprAIEmbed() 
{
}

int ObExprAIEmbed::calc_result_typeN(ObExprResType &type,
                                     ObExprResType *types_stack,
                                     int64_t param_num,
                                     common::ObExprTypeCtx &type_ctx) const 
{
  UNUSED(type_ctx);
  UNUSED(types_stack);
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(param_num > 3 || param_num < 2)) {
    ObString func_name_(get_name());
    ret = OB_ERR_PARAM_SIZE;
    LOG_USER_ERROR(OB_ERR_PARAM_SIZE, func_name_.length(), func_name_.ptr());
  } else {
    types_stack[MODEL_IDX].set_calc_type(ObVarcharType);
    types_stack[MODEL_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    types_stack[CONTENT_IDX].set_calc_type(ObVarcharType);
    types_stack[CONTENT_IDX].set_calc_collation_type(CS_TYPE_UTF8MB4_BIN);
    if (param_num == 3) {
      if (ob_is_integer_type(types_stack[DIM_IDX].get_type())) {
        types_stack[DIM_IDX].set_calc_type(ObIntType);
        types_stack[DIM_IDX].set_precision(10);
        types_stack[DIM_IDX].set_scale(0);
      } else {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be an integer, not a decimal or float");
      }
    }
    type.set_varchar();
    type.set_collation_type(CS_TYPE_UTF8MB4_BIN);
    type.set_collation_level(CS_LEVEL_COERCIBLE);
  }
  return ret;
}

int ObExprAIEmbed::eval_ai_embed(const ObExpr &expr, ObEvalCtx &ctx, ObDatum &res) 
{
  INIT_SUCC(ret);
  ObDatum *arg_model_id = nullptr;
  ObDatum *arg_content = nullptr;
  ObDatum *arg_dim = nullptr;
  if (expr.arg_cnt_ == 3 ? OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content, arg_dim))
                         : OB_FAIL(expr.eval_param_value(ctx, arg_model_id, arg_content))) {
  } else if (arg_model_id->is_null()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
    res.set_null();
  } else if (arg_content->is_null()) {
    // A NULL input row yields a NULL embedding: it must not be batched into the
    // request, and it must not fail the whole query.
    res.set_null();
  } else {
    ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
    
    MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
    lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
    ObAIFuncExprInfo *info = nullptr;
    share::ObAiModelEndpointInfo resolved_endpoint;
    const share::ObAiModelEndpointInfo *endpoint_info = &resolved_endpoint;
    query::ObIAiEndpointResolver *endpoint_resolver =
        ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();
    ObString model_id = arg_model_id->get_string();
    ObString content = arg_content->get_string();
    if (model_id.empty() || content.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
      res.set_null();
    }
    int64_t dim = 0;
    ObJsonInt *dim_json = nullptr;
    ObJsonObject *config = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_NOT_NULL(arg_dim)) {
      dim = arg_dim->get_int();
      if (dim <= 0) {
        ret = OB_INVALID_ARGUMENT;
        LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, dimension parameter must be a positive integer");
        res.set_null();
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
      } else if (OB_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, dim, dim_json))) {
      } else if (OB_FAIL(config->add("dimensions", dim_json))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, model_id, info))) {
    } else if (OB_ISNULL(endpoint_resolver)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(endpoint_resolver->resolve_by_model_name(
                   model_id, temp_allocator, resolved_endpoint))) {
    } else {
      ObAIFuncModel model(temp_allocator, *info, *endpoint_info);
      ObString result;
      if (OB_FAIL(model.call_dense_embedding(content, config, result))) {
      } else if (OB_FAIL(ObAIFuncUtils::set_string_result(expr, ctx, res, result))) {
      }
    }
  }
  return ret;
}

// Cross-row batch evaluation.  Every live row of the batch is validated exactly
// like eval_ai_embed does for one row, then the survivors are grouped by
// (model_id, dimension).  A group is flushed by ONE
// call_dense_embedding_vector_v2, so a query over N rows issues
// ceil(N / MAX_EMBED_INPUTS_PER_REQUEST) requests instead of N.
//
// Row alignment is structural, never positional: a result is always written to
// res[original_row_index], and NULL inputs are neither sent nor counted, so a
// NULL row cannot shift the rows around it.  Results carry the provider's
// input `index`, which ObOpenAIEmbed::parse_output has already used to restore
// input order, so an out-of-order response is scattered correctly too.
int ObExprAIEmbed::eval_ai_embed_batch(const ObExpr &expr, ObEvalCtx &ctx,
                                       const ObBitVector &skip,
                                       const int64_t batch_size)
{
  int ret = OB_SUCCESS;
  ObDatum *res = nullptr;
  if (OB_UNLIKELY(expr.arg_cnt_ < 2 || expr.arg_cnt_ > 3) || OB_ISNULL(expr.args_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected ai_embed argument count", K(ret), K(expr.arg_cnt_));
  } else if (OB_ISNULL(res = expr.locate_batch_datums(ctx))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected null batch datum array", K(ret));
  } else {
    const int64_t arg_cnt = expr.arg_cnt_;
    ObBitVector &eval_flags = expr.get_evaluated_flags(ctx);
    // Materialise every argument for the whole batch first: grouping needs the
    // model id, the content and the dimension of every live row.
    for (int64_t i = 0; OB_SUCC(ret) && i < arg_cnt; ++i) {
      if (OB_FAIL(expr.args_[i]->eval_batch(ctx, skip, batch_size))) {
        LOG_WARN("failed to eval ai_embed argument batch", K(ret), K(i));
      }
    }
    if (OB_SUCC(ret)) {
      ObDatumVector model_ids = expr.args_[MODEL_IDX]->locate_expr_datumvector(ctx);
      ObDatumVector contents = expr.args_[CONTENT_IDX]->locate_expr_datumvector(ctx);
      ObDatumVector dims;
      if (arg_cnt == 3) {
        dims = expr.args_[DIM_IDX]->locate_expr_datumvector(ctx);
      }
      ObEvalCtx::TempAllocGuard tmp_alloc_g(ctx);
      MultimodeAlloctor temp_allocator(tmp_alloc_g.get_allocator());
      lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr(N_AI_EMBED));
      ObEvalCtx::BatchInfoScopeGuard batch_info_guard(ctx);
      batch_info_guard.set_batch_size(batch_size);
      query::ObIAiEndpointResolver *endpoint_resolver =
          ::oceanbase::share::server_service<::oceanbase::query::ObIAiEndpointResolver>();

      // A group is one (model_id, dimension) pair.  Its rows travel in one
      // request; a changed pair or a full chunk flushes it.  The group identity
      // and the resolved schema/endpoint survive a chunk flush, so a run of
      // rows on the same pair resolves the schema and the endpoint once.
      ObString cur_model_id;
      int64_t cur_dim = 0;
      bool have_group = false;
      bool group_resolved = false;
      ObAIFuncExprInfo *cur_info = nullptr;
      share::ObAiModelEndpointInfo cur_endpoint;
      ObArray<int64_t> group_rows;
      ObArray<ObString> group_texts;

      auto flush_group = [&]() -> int {
        int tmp_ret = OB_SUCCESS;
        ObJsonInt *dim_json = nullptr;
        ObJsonObject *config = nullptr;
        if (group_texts.empty()) {
          // nothing to send
        } else if (!group_resolved) {
          if (OB_TMP_FAIL(ObAIFuncUtils::get_ai_func_info(temp_allocator, cur_model_id, cur_info))) {
            LOG_WARN("failed to get ai func info", K(tmp_ret), K(cur_model_id));
          } else if (OB_ISNULL(endpoint_resolver)) {
            tmp_ret = OB_ERR_UNEXPECTED;
            LOG_WARN("ai endpoint resolver is null", K(tmp_ret));
          } else if (OB_TMP_FAIL(endpoint_resolver->resolve_by_model_name(
                         cur_model_id, temp_allocator, cur_endpoint))) {
            LOG_WARN("failed to resolve ai model endpoint", K(tmp_ret), K(cur_model_id));
          } else {
            group_resolved = true;
          }
        }
        if (OB_SUCCESS != tmp_ret) {
        } else if (cur_dim > 0) {
          if (OB_TMP_FAIL(ObAIFuncJsonUtils::get_json_object(temp_allocator, config))) {
          } else if (OB_TMP_FAIL(ObAIFuncJsonUtils::get_json_int(temp_allocator, cur_dim, dim_json))) {
          } else if (OB_TMP_FAIL(config->add("dimensions", dim_json))) {
          }
        }
        if (OB_SUCCESS != tmp_ret) {
        } else if (OB_ISNULL(cur_info)) {
          tmp_ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai func info is null", K(tmp_ret));
        } else {
          ObAIFuncModel model(temp_allocator, *cur_info, cur_endpoint);
          ObArray<ObString> results;
          // Exactly one HTTP request for the whole chunk.
          if (OB_TMP_FAIL(model.call_dense_embedding_vector_v2(group_texts, config, results))) {
            LOG_WARN("failed to call dense embedding vector v2", K(tmp_ret), K(group_texts.count()));
          } else if (results.count() != group_texts.count()) {
            tmp_ret = OB_ERR_UNEXPECTED;
            LOG_WARN("unexpected embedding result count", K(tmp_ret),
                     K(results.count()), K(group_texts.count()));
          } else {
            for (int64_t k = 0; OB_SUCCESS == tmp_ret && k < group_rows.count(); ++k) {
              const int64_t row = group_rows.at(k);
              // Routes get_str_res_mem()/get_datum_idx() to this row's slot.
              batch_info_guard.set_batch_idx(row);
              if (OB_TMP_FAIL(ObAIFuncUtils::set_string_result(
                      expr, ctx, res[row], results.at(k)))) {
                LOG_WARN("failed to set string result", K(tmp_ret), K(row));
              } else {
                eval_flags.set(row);
              }
            }
          }
        }
        group_rows.reuse();
        group_texts.reuse();
        return tmp_ret;
      };

      for (int64_t j = 0; OB_SUCC(ret) && j < batch_size; ++j) {
        if (skip.at(j) || eval_flags.at(j)) {
          continue;
        }
        ObDatum *model_datum = model_ids.at(j);
        ObDatum *content_datum = contents.at(j);
        if (OB_ISNULL(model_datum) || OB_ISNULL(content_datum)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("unexpected null argument datum", K(ret), K(j));
        } else if (model_datum->is_null()) {
          ret = OB_INVALID_ARGUMENT;
          LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id is null");
          res[j].set_null();
        } else if (content_datum->is_null()) {
          // Single-row parity: a NULL input row yields a NULL embedding, is
          // never sent in a request and never fails the query.
          res[j].set_null();
          eval_flags.set(j);
        } else {
          ObString model_id = model_datum->get_string();
          ObString content = content_datum->get_string();
          int64_t dim = 0;
          if (model_id.empty() || content.empty()) {
            ret = OB_INVALID_ARGUMENT;
            LOG_USER_ERROR(OB_INVALID_ARGUMENT, "ai_embed, model id or input is empty");
            res[j].set_null();
          } else if (arg_cnt == 3 && !dims.at(j)->is_null()) {
            dim = dims.at(j)->get_int();
            if (dim <= 0) {
              ret = OB_INVALID_ARGUMENT;
              LOG_USER_ERROR(OB_INVALID_ARGUMENT,
                             "ai_embed, dimension parameter must be a positive integer");
              res[j].set_null();
            }
          }
          if (OB_FAIL(ret)) {
          } else if (have_group && (dim != cur_dim || model_id != cur_model_id)) {
            if (OB_FAIL(flush_group())) {
              LOG_WARN("failed to flush ai_embed group", K(ret));
            } else {
              have_group = false;
              group_resolved = false;
              cur_info = nullptr;
            }
          }
          if (OB_FAIL(ret)) {
          } else if (!have_group) {
            cur_model_id = model_id;
            cur_dim = dim;
            have_group = true;
            group_resolved = false;
            cur_info = nullptr;
          }
          if (OB_SUCC(ret)) {
            if (OB_FAIL(group_rows.push_back(j))) {
              LOG_WARN("failed to push back ai_embed row index", K(ret), K(j));
            } else if (OB_FAIL(group_texts.push_back(content))) {
              LOG_WARN("failed to push back ai_embed content", K(ret), K(j));
            } else if (group_texts.count() >= MAX_EMBED_INPUTS_PER_REQUEST) {
              if (OB_FAIL(flush_group())) {
                LOG_WARN("failed to flush ai_embed chunk", K(ret));
              }
            }
          }
        }
      }
      // Trailing partial chunk.  Any error above has already stopped the query.
      if (OB_SUCC(ret) && !group_texts.empty()) {
        if (OB_FAIL(flush_group())) {
          LOG_WARN("failed to flush trailing ai_embed group", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObExprAIEmbed::cg_expr(ObExprCGCtx &expr_cg_ctx, 
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
  //   ObObj const_data;
  //   bool got_data = false;
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
  //   } else if (OB_FAIL(ObAIFuncUtils::get_ai_func_info(*allocator, const_data.get_string(), *expr_cg_ctx.schema_guard_, info))) {
  //     LOG_WARN("failed to get ai func info", K(ret), K(const_data.get_string()));
  //   } else {
  //     rt_expr.extra_info_ = info;
  //   }
  // }

  if (OB_SUCC(ret)) {
    // Both callbacks: all-const arguments are not vectorizable, so they keep the
    // untouched row-at-a-time path; a batch-result expression uses the batch one.
    rt_expr.eval_func_ = ObExprAIEmbed::eval_ai_embed;
    rt_expr.eval_batch_func_ = ObExprAIEmbed::eval_ai_embed_batch;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
