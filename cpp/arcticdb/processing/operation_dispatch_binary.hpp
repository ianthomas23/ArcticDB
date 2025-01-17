/*
 * Copyright 2023 Man Group Operations Limited
 *
 * Use of this software is governed by the Business Source License 1.1 included in the file licenses/BSL.txt.
 *
 * As of the Change Date specified in that file, in accordance with the Business Source License, use of this software will be governed by the Apache License, version 2.0.
 */

#pragma once

#include <cstdint>
#include <variant>
#include <memory>
#include <type_traits>

#include <folly/futures/Future.h>

#include <arcticdb/pipeline/value.hpp>
#include <arcticdb/pipeline/value_set.hpp>
#include <arcticdb/column_store/column.hpp>
#include <arcticdb/column_store/memory_segment.hpp>
#include <arcticdb/util/variant.hpp>
#include <arcticdb/entity/types.hpp>
#include <arcticdb/entity/type_utils.hpp>
#include <arcticdb/processing/operation_dispatch.hpp>
#include <arcticdb/processing/expression_node.hpp>
#include <arcticdb/entity/type_conversion.hpp>

namespace arcticdb {

VariantData binary_boolean(const util::BitSet& left, const util::BitSet& right, OperationType operation);

VariantData binary_boolean(const util::BitSet& left, EmptyResult, OperationType operation);

VariantData binary_boolean(const util::BitSet& left, FullResult, OperationType operation);

VariantData binary_boolean(EmptyResult, FullResult, OperationType operation);

VariantData binary_boolean(FullResult, FullResult, OperationType operation);

VariantData binary_boolean(EmptyResult, EmptyResult, OperationType operation);

// Note that we can have fewer of these and reverse the parameters because all the operations are
// commutative, however if that were to change we would need the full set
VariantData visit_binary_boolean(const VariantData& left, const VariantData& right, OperationType operation);

template <typename Func>
VariantData binary_membership(const ColumnWithStrings& column_with_strings, ValueSet& value_set, Func&& func) {
    if (is_empty_type(column_with_strings.column_->type().data_type())) {
        if constexpr(std::is_same_v<Func, IsInOperator&&>) {
            return EmptyResult{};
        } else if constexpr(std::is_same_v<Func, IsNotInOperator&&>) {
            return FullResult{};
        } else {
            internal::raise<ErrorCode::E_ASSERTION_FAILURE>("Unexpected operator passed to binary_membership");
        }
    }
    util::BitSet output_bitset(column_with_strings.column_->row_count());
    // If the value set is empty, then for IsInOperator, we are done
    // For the IsNotInOperator, set all bits to 1
    if (value_set.empty()) {
        if constexpr(std::is_same_v<Func, IsNotInOperator&&>)
            output_bitset.set();
    } else {
        details::visit_type(column_with_strings.column_->type().data_type(),[&] (auto col_desc_tag) {
            using col_type_info = ScalarTypeInfo<decltype(col_desc_tag)>;
            details::visit_type(value_set.base_type().data_type(), [&] (auto val_set_desc_tag) {
                using val_set_type_info = ScalarTypeInfo<decltype(val_set_desc_tag)>;
                if constexpr(is_sequence_type(col_type_info::data_type) && is_sequence_type(val_set_type_info::data_type)) {
                    std::shared_ptr<std::unordered_set<std::string>> typed_value_set;
                    if constexpr(is_fixed_string_type(col_type_info::data_type)) {
                        auto width = column_with_strings.get_fixed_width_string_size();
                        if (width.has_value()) {
                            typed_value_set = value_set.get_fixed_width_string_set(*width);
                        }
                    } else {
                        typed_value_set = value_set.get_set<std::string>();
                    }
                    auto offset_set = column_with_strings.string_pool_->get_offsets_for_column(typed_value_set, *column_with_strings.column_);
                    Column::transform<typename col_type_info::TDT>(*column_with_strings.column_, output_bitset, [&func, &offset_set](auto input_value) -> bool {
                        auto offset = static_cast<entity::position_t>(input_value);
                        return func(offset, offset_set);
                    });
                } else if constexpr (is_bool_type(col_type_info::data_type) && is_bool_type(val_set_type_info::data_type)) {
                    util::raise_rte("Binary membership not implemented for bools");
                } else if constexpr (is_numeric_type(col_type_info::data_type) && is_numeric_type(val_set_type_info::data_type)) {
                    using WideType = typename type_arithmetic_promoted_type<typename col_type_info::RawType, typename val_set_type_info::RawType, std::remove_reference_t<Func>>::type;
                    auto typed_value_set = value_set.get_set<WideType>();
                    Column::transform<typename col_type_info::TDT>(*column_with_strings.column_, output_bitset, [&func, &typed_value_set](auto input_value) -> bool {
                        if constexpr (MembershipOperator::needs_uint64_special_handling<typename col_type_info::RawType, typename val_set_type_info::RawType>) {
                            // Avoid narrowing conversion on *input_it:
                            return func(input_value, *typed_value_set, UInt64SpecialHandlingTag{});
                        } else {
                            return func(static_cast<WideType>(input_value), *typed_value_set);
                        }
                    });
                } else {
                    util::raise_rte("Cannot check membership of {} in set of {} (possible categorical?)",
                                    column_with_strings.column_->type(), value_set.base_type());
                }
            });
        });
    }

    output_bitset.optimize();
    auto pos = 0;

    if(column_with_strings.column_->is_sparse()) {
        const auto& sparse_map = column_with_strings.column_->opt_sparse_map().value();
        util::BitSet replace(sparse_map.size());
        auto it = sparse_map.first();
        auto it_end = sparse_map.end();
        while(it < it_end) {
           replace.set(*it, output_bitset.test(pos++));
            ++it;
        }
    }

    log::version().debug("Filtered segment of size {} down to {} bits", output_bitset.size(), output_bitset.count());

    return {std::move(output_bitset)};
}

template<typename Func>
VariantData visit_binary_membership(const VariantData &left, const VariantData &right, Func &&func) {
    if (std::holds_alternative<EmptyResult>(left))
        return EmptyResult{};

    return std::visit(util::overload {
        [&] (const ColumnWithStrings& l, const std::shared_ptr<ValueSet>& r) ->VariantData  {
            return transform_to_placeholder(binary_membership<decltype(func)>(l, *r, std::forward<decltype(func)>(func)));
            },
            [](const auto &, const auto&) -> VariantData {
            util::raise_rte("Binary membership operations must be Column/ValueSet");
        }
        }, left, right);
}

template <typename Func>
VariantData binary_comparator(const ColumnWithStrings& left, const ColumnWithStrings& right, Func&& func) {
    if (is_empty_type(left.column_->type().data_type()) || is_empty_type(right.column_->type().data_type())) {
        return EmptyResult{};
    }
    util::check(left.column_->row_count() == right.column_->row_count(), "Columns with different row counts ({} and {}) in binary comparator", left.column_->row_count(), right.column_->row_count());
    util::BitSet output_bitset(static_cast<util::BitSetSizeType>(left.column_->row_count()));

    details::visit_type(left.column_->type().data_type(), [&](auto left_desc_tag) {
        using left_type_info = ScalarTypeInfo<decltype(left_desc_tag)>;
        details::visit_type(right.column_->type().data_type(), [&](auto right_desc_tag) {
            using right_type_info = ScalarTypeInfo<decltype(right_desc_tag)>;
            if constexpr(is_sequence_type(left_type_info::data_type) && is_sequence_type(right_type_info::data_type)) {
                bool strip_fixed_width_trailing_nulls{false};
                // If one or both columns are fixed width strings, we need to strip trailing null characters to get intuitive results
                if constexpr (is_fixed_string_type(left_type_info::data_type) || is_fixed_string_type(right_type_info::data_type)) {
                    strip_fixed_width_trailing_nulls = true;
                }
                Column::transform<typename left_type_info::TDT, typename right_type_info::TDT>(*left.column_, *right.column_, output_bitset, [&func, &left, &right, strip_fixed_width_trailing_nulls] (auto left_value, auto right_value) -> bool {
                    return func(left.string_at_offset(left_value, strip_fixed_width_trailing_nulls), right.string_at_offset(right_value, strip_fixed_width_trailing_nulls));
                });
            } else if constexpr ((is_numeric_type(left_type_info::data_type) && is_numeric_type(right_type_info::data_type)) ||
                                 (is_bool_type(left_type_info::data_type) && is_bool_type(right_type_info::data_type))) {
                using comp = typename arcticdb::Comparable<typename left_type_info::RawType, typename right_type_info::RawType>;
                Column::transform<typename left_type_info::TDT, typename right_type_info::TDT>(*left.column_, *right.column_, output_bitset, [&func] (auto left_value, auto right_value) -> bool {
                    return func(static_cast<typename comp::left_type>(left_value), static_cast<typename comp::right_type>(right_value));
                });
            } else {
                util::raise_rte("Cannot compare {} to {} (possible categorical?)", left.column_->type(), right.column_->type());
            }
        });
    });
    ARCTICDB_DEBUG(log::version(), "Filtered segment of size {} down to {} bits", output_bitset.size(), output_bitset.count());

    return VariantData{std::move(output_bitset)};
}

template <typename Func, bool arguments_reversed = false>
VariantData binary_comparator(const ColumnWithStrings& column_with_strings, const Value& val, Func&& func) {
    if (is_empty_type(column_with_strings.column_->type().data_type())) {
        return EmptyResult{};
    }
    util::BitSet output_bitset(static_cast<util::BitSetSizeType>(column_with_strings.column_->row_count()));

    details::visit_type(column_with_strings.column_->type().data_type(), [&](auto col_desc_tag) {
        using col_type_info = ScalarTypeInfo<decltype(col_desc_tag)>;
        details::visit_type(val.type().data_type(), [&](auto val_desc_tag) {
            using val_type_info = ScalarTypeInfo<decltype(val_desc_tag)>;
            if constexpr(is_sequence_type(col_type_info::data_type) && is_sequence_type(val_type_info::data_type)) {
                std::optional<std::string> utf32_string;
                std::string value_string;
                if constexpr(is_fixed_string_type(col_type_info::data_type)) {
                    auto width = column_with_strings.get_fixed_width_string_size();
                    if (width.has_value()) {
                        utf32_string = ascii_to_padded_utf32(std::string_view(*val.str_data(), val.len()), *width);
                        if (utf32_string.has_value()) {
                            value_string = *utf32_string;
                        }
                    }
                } else {
                    value_string = std::string(*val.str_data(), val.len());
                }
                auto value_offset = column_with_strings.string_pool_->get_offset_for_column(value_string, *column_with_strings.column_);
                Column::transform<typename col_type_info::TDT>(*column_with_strings.column_, output_bitset, [&func, value_offset](auto input_value) -> bool {
                    auto offset = static_cast<entity::position_t>(input_value);
                    if constexpr (arguments_reversed) {
                        return func(offset, value_offset);
                    } else {
                        return func(value_offset, offset);
                    }
                });
            } else if constexpr ((is_numeric_type(col_type_info::data_type) && is_numeric_type(val_type_info::data_type)) ||
                                 (is_bool_type(col_type_info::data_type) && is_bool_type(val_type_info::data_type))) {
                using comp = std::conditional_t<arguments_reversed,
                                                typename arcticdb::Comparable<typename col_type_info::RawType, typename val_type_info::RawType>,
                                                typename arcticdb::Comparable<typename val_type_info::RawType, typename col_type_info::RawType>>;
                auto value = static_cast<typename comp::left_type>(*reinterpret_cast<const typename val_type_info::RawType *>(val.data_));
                Column::transform<typename col_type_info::TDT>(*column_with_strings.column_, output_bitset, [&func, value](auto input_value) -> bool {
                    if constexpr (arguments_reversed) {
                        return func(value, static_cast<typename comp::right_type>(input_value));
                    } else {
                        return func(static_cast<typename comp::right_type>(input_value), value);
                    }
                });

            } else {
                util::raise_rte("Cannot compare {} to {} (possible categorical?)", column_with_strings.column_->type(), val.type());
            }
        });
    });
    ARCTICDB_DEBUG(log::version(), "Filtered segment of size {} down to {} bits", output_bitset.size(), output_bitset.count());

    return VariantData{std::move(output_bitset)};
}

template<typename Func>
VariantData visit_binary_comparator(const VariantData& left, const VariantData& right, Func&& func) {
    if(std::holds_alternative<EmptyResult>(left) || std::holds_alternative<EmptyResult>(right))
        return EmptyResult{};

    return std::visit(util::overload {
     [&func] (const ColumnWithStrings& l, const std::shared_ptr<Value>& r) ->VariantData  {
        auto result = binary_comparator<decltype(func)>(l, *r, std::forward<decltype(func)>(func));
         return transform_to_placeholder(result);
        },
        [&] (const ColumnWithStrings& l, const ColumnWithStrings& r)  ->VariantData {
        auto result = binary_comparator<decltype(func)>(l, r, std::forward<decltype(func)>(func));
        return transform_to_placeholder(result);
        },
        [&](const std::shared_ptr<Value>& l, const ColumnWithStrings& r) ->VariantData {
        auto result =  binary_comparator<decltype(func), true>(r, *l, std::forward<decltype(func)>(func));
        return transform_to_placeholder(result);
        },
        [&] ([[maybe_unused]] const std::shared_ptr<Value>& l, [[maybe_unused]] const std::shared_ptr<Value>& r) ->VariantData  {
        util::raise_rte("Two value inputs not accepted to binary comparators");
        },
        [](const auto &, const auto&) -> VariantData {
        util::raise_rte("Bitset/ValueSet inputs not accepted to binary comparators");
    }
    }, left, right);
}

template <typename Func>
VariantData binary_operator(const Value& left, const Value& right, Func&& func) {
    auto output_value = std::make_unique<Value>();

    details::visit_type(left.type().data_type(), [&](auto left_desc_tag) {
        using left_type_info = ScalarTypeInfo<decltype(left_desc_tag)>;
        if constexpr(!is_numeric_type(left_type_info::data_type)) {
            util::raise_rte("Non-numeric type provided to binary operation: {}", left.type());
        }
        auto left_value = *reinterpret_cast<const typename left_type_info::RawType*>(left.data_);
        details::visit_type(right.type().data_type(), [&](auto right_desc_tag) {
            using right_type_info = ScalarTypeInfo<decltype(right_desc_tag)>;
            if constexpr(!is_numeric_type(right_type_info::data_type)) {
                util::raise_rte("Non-numeric type provided to binary operation: {}", right.type());
            }
            auto right_value = *reinterpret_cast<const typename right_type_info::RawType*>(right.data_);
            using TargetType = typename type_arithmetic_promoted_type<typename left_type_info::RawType, typename right_type_info::RawType, std::remove_reference_t<Func>>::type;
            output_value->data_type_ = data_type_from_raw_type<TargetType>();
            *reinterpret_cast<TargetType*>(output_value->data_) = func.apply(left_value, right_value);
        });
    });
    return VariantData(std::move(output_value));
}

template <typename Func>
VariantData binary_operator(const Column& left, const Column& right, Func&& func) {
    schema::check<ErrorCode::E_UNSUPPORTED_COLUMN_TYPE>(
            !is_empty_type(left.type().data_type()) && !is_empty_type(right.type().data_type()),
            "Empty column provided to binary operator");
    util::check(left.row_count() == right.row_count(), "Columns with different row counts ({} and {}) in binary operator", left.row_count(), right.row_count());
    std::unique_ptr<Column> output_column;

    details::visit_type(left.type().data_type(), [&](auto left_desc_tag) {
        using left_type_info = ScalarTypeInfo<decltype(left_desc_tag)>;
        if constexpr(!is_numeric_type(left_type_info::data_type)) {
            util::raise_rte("Non-numeric type provided to binary operation: {}", left.type());
        }
        details::visit_type(right.type().data_type(), [&](auto right_desc_tag) {
            using right_type_info = ScalarTypeInfo<decltype(right_desc_tag)>;
            if constexpr(!is_numeric_type(right_type_info::data_type)) {
                util::raise_rte("Non-numeric type provided to binary operation: {}", right.type());
            }
            using TargetType = typename type_arithmetic_promoted_type<typename left_type_info::RawType, typename right_type_info::RawType, std::remove_reference_t<decltype(func)>>::type;
            constexpr auto output_data_type = data_type_from_raw_type<TargetType>();
            output_column = std::make_unique<Column>(make_scalar_type(output_data_type), left.row_count(), true, false);
            output_column->set_row_data(left.last_row());
            Column::transform<typename left_type_info::TDT, typename right_type_info::TDT, ScalarTagType<DataTypeTag<output_data_type>>>(
                    left, right, *output_column, [&func] (auto left_value, auto right_value) -> TargetType {
                        return func.apply(left_value, right_value);
                    });
        });
    });
    return VariantData(ColumnWithStrings(std::move(output_column)));
}

template <typename Func, bool arguments_reversed = false>
VariantData binary_operator(const Column& col, const Value& val, Func&& func) {
    schema::check<ErrorCode::E_UNSUPPORTED_COLUMN_TYPE>(
            !is_empty_type(col.type().data_type()),
            "Empty column provided to binary operator");
    std::unique_ptr<Column> output_column;

    details::visit_type(col.type().data_type(), [&](auto col_desc_tag) {
        using col_type_info = ScalarTypeInfo<decltype(col_desc_tag)>;
        if constexpr(!is_numeric_type(col_type_info::data_type)) {
            util::raise_rte("Non-numeric type provided to binary operation: {}", col.type());
        }
        details::visit_type(val.type().data_type(), [&](auto val_desc_tag) {
            using val_type_info = ScalarTypeInfo<decltype(val_desc_tag)>;
            if constexpr(!is_numeric_type(val_type_info::data_type)) {
                util::raise_rte("Non-numeric type provided to binary operation: {}", val.type());
            }
            auto raw_value = *reinterpret_cast<const typename val_type_info::RawType*>(val.data_);
            using TargetType = typename type_arithmetic_promoted_type<typename col_type_info::RawType, typename val_type_info::RawType, std::remove_reference_t<decltype(func)>>::type;
            using ReversedTargetType = typename type_arithmetic_promoted_type<typename val_type_info::RawType, typename col_type_info::RawType, std::remove_reference_t<decltype(func)>>::type;
            if constexpr(arguments_reversed) {
                constexpr auto output_data_type = data_type_from_raw_type<ReversedTargetType>();
                output_column = std::make_unique<Column>(make_scalar_type(output_data_type), col.row_count(), true, false);
                output_column->set_row_data(col.last_row());
                Column::transform<typename col_type_info ::TDT, ScalarTagType<DataTypeTag<output_data_type>>>(col, *output_column, [&func, raw_value](auto input_value) -> ReversedTargetType {
                    return func.apply(raw_value, input_value);
                });
            } else {
                constexpr auto output_data_type = data_type_from_raw_type<TargetType>();
                output_column = std::make_unique<Column>(make_scalar_type(output_data_type), col.row_count(), true, false);
                output_column->set_row_data(col.last_row());
                Column::transform<typename col_type_info ::TDT, ScalarTagType<DataTypeTag<output_data_type>>>(col, *output_column, [&func, raw_value](auto input_value) -> TargetType {
                    return func.apply(input_value, raw_value);
                });
            }
        });
    });
    return {ColumnWithStrings(std::move(output_column))};
}

template<typename Func>
VariantData visit_binary_operator(const VariantData& left, const VariantData& right, Func&& func) {
    if(std::holds_alternative<EmptyResult>(left) || std::holds_alternative<EmptyResult>(right))
        return EmptyResult{};

    return std::visit(util::overload {
        [&] (const ColumnWithStrings& l, const std::shared_ptr<Value>& r) ->VariantData  {
            return binary_operator<decltype(func)>(*(l.column_), *r, std::forward<decltype(func)>(func));
            },
            [&] (const ColumnWithStrings& l, const ColumnWithStrings& r)  ->VariantData {
            return binary_operator<decltype(func)>(*(l.column_), *(r.column_), std::forward<decltype(func)>(func));
            },
            [&](const std::shared_ptr<Value>& l, const ColumnWithStrings& r) ->VariantData {
            return binary_operator<decltype(func), true>(*(r.column_), *l, std::forward<decltype(func)>(func));
            },
            [&] (const std::shared_ptr<Value>& l, const std::shared_ptr<Value>& r) -> VariantData {
            return binary_operator<decltype(func)>(*l, *r, std::forward<decltype(func)>(func));
            },
            [](const auto &, const auto&) -> VariantData {
            util::raise_rte("Bitset/ValueSet inputs not accepted to binary operators");
        }
        }, left, right);
}

VariantData dispatch_binary(const VariantData& left, const VariantData& right, OperationType operation);

// instantiated in operation_dispatch_binary_operator.cpp to reduce compilation memory use
extern template
VariantData visit_binary_operator<arcticdb::PlusOperator>(const VariantData&, const VariantData&, PlusOperator&&);
extern template
VariantData visit_binary_operator<arcticdb::MinusOperator>(const VariantData&, const VariantData&, MinusOperator&&);
extern template
VariantData visit_binary_operator<arcticdb::TimesOperator>(const VariantData&, const VariantData&, TimesOperator&&);
extern template
VariantData visit_binary_operator<arcticdb::DivideOperator>(const VariantData&, const VariantData&, DivideOperator&&);

// instantiated in operation_dispatch_binary_comparator.cpp to reduce compilation memory use
extern template
VariantData visit_binary_comparator<EqualsOperator>(const VariantData&, const VariantData&, EqualsOperator&&);
extern template
VariantData visit_binary_comparator<NotEqualsOperator>(const VariantData&, const VariantData&, NotEqualsOperator&&);
extern template
VariantData visit_binary_comparator<LessThanOperator>(const VariantData&, const VariantData&, LessThanOperator&&);
extern template
VariantData visit_binary_comparator<LessThanEqualsOperator>(const VariantData&, const VariantData&, LessThanEqualsOperator&&);
extern template
VariantData visit_binary_comparator<GreaterThanOperator>(const VariantData&, const VariantData&, GreaterThanOperator&&);
extern template
VariantData visit_binary_comparator<GreaterThanEqualsOperator>(const VariantData&, const VariantData&, GreaterThanEqualsOperator&&);

}
