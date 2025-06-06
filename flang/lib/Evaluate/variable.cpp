//===-- lib/Evaluate/variable.cpp -----------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "flang/Evaluate/variable.h"
#include "flang/Common/idioms.h"
#include "flang/Evaluate/check-expression.h"
#include "flang/Evaluate/fold.h"
#include "flang/Evaluate/tools.h"
#include "flang/Parser/char-block.h"
#include "flang/Parser/characters.h"
#include "flang/Parser/message.h"
#include "flang/Semantics/scope.h"
#include "flang/Semantics/symbol.h"
#include <type_traits>

using namespace Fortran::parser::literals;

namespace Fortran::evaluate {

// Constructors, accessors, mutators

Triplet::Triplet() : stride_{Expr<SubscriptInteger>{1}} {}

Triplet::Triplet(std::optional<Expr<SubscriptInteger>> &&l,
    std::optional<Expr<SubscriptInteger>> &&u,
    std::optional<Expr<SubscriptInteger>> &&s)
    : stride_{s ? std::move(*s) : Expr<SubscriptInteger>{1}} {
  if (l) {
    lower_.emplace(std::move(*l));
  }
  if (u) {
    upper_.emplace(std::move(*u));
  }
}

std::optional<Expr<SubscriptInteger>> Triplet::lower() const {
  if (lower_) {
    return {lower_.value().value()};
  }
  return std::nullopt;
}

Triplet &Triplet::set_lower(Expr<SubscriptInteger> &&expr) {
  lower_.emplace(std::move(expr));
  return *this;
}

std::optional<Expr<SubscriptInteger>> Triplet::upper() const {
  if (upper_) {
    return {upper_.value().value()};
  }
  return std::nullopt;
}

Triplet &Triplet::set_upper(Expr<SubscriptInteger> &&expr) {
  upper_.emplace(std::move(expr));
  return *this;
}

Expr<SubscriptInteger> Triplet::stride() const { return stride_.value(); }

Triplet &Triplet::set_stride(Expr<SubscriptInteger> &&expr) {
  stride_.value() = std::move(expr);
  return *this;
}

CoarrayRef::CoarrayRef(
    DataRef &&base, std::vector<Expr<SubscriptInteger>> &&css)
    : base_{std::move(base)}, cosubscript_(std::move(css)) {}

std::optional<Expr<SomeInteger>> CoarrayRef::stat() const {
  if (stat_) {
    return stat_.value().value();
  } else {
    return std::nullopt;
  }
}

std::optional<Expr<SomeType>> CoarrayRef::team() const {
  if (team_) {
    return team_.value().value();
  } else {
    return std::nullopt;
  }
}

CoarrayRef &CoarrayRef::set_stat(Expr<SomeInteger> &&v) {
  CHECK(IsVariable(v));
  stat_.emplace(std::move(v));
  return *this;
}

CoarrayRef &CoarrayRef::set_team(Expr<SomeType> &&v) {
  team_.emplace(std::move(v));
  return *this;
}

const Symbol &CoarrayRef::GetFirstSymbol() const {
  return base().GetFirstSymbol();
}

const Symbol &CoarrayRef::GetLastSymbol() const {
  return base().GetLastSymbol();
}

void Substring::SetBounds(std::optional<Expr<SubscriptInteger>> &lower,
    std::optional<Expr<SubscriptInteger>> &upper) {
  if (lower) {
    set_lower(std::move(lower.value()));
  }
  if (upper) {
    set_upper(std::move(upper.value()));
  }
}

Expr<SubscriptInteger> Substring::lower() const {
  if (lower_) {
    return lower_.value().value();
  } else {
    return AsExpr(Constant<SubscriptInteger>{1});
  }
}

Substring &Substring::set_lower(Expr<SubscriptInteger> &&expr) {
  lower_.emplace(std::move(expr));
  return *this;
}

std::optional<Expr<SubscriptInteger>> Substring::upper() const {
  if (upper_) {
    return upper_.value().value();
  } else {
    return common::visit(
        common::visitors{
            [](const DataRef &dataRef) { return dataRef.LEN(); },
            [](const StaticDataObject::Pointer &object)
                -> std::optional<Expr<SubscriptInteger>> {
              return AsExpr(Constant<SubscriptInteger>{object->data().size()});
            },
        },
        parent_);
  }
}

Substring &Substring::set_upper(Expr<SubscriptInteger> &&expr) {
  upper_.emplace(std::move(expr));
  return *this;
}

std::optional<Expr<SomeCharacter>> Substring::Fold(FoldingContext &context) {
  if (!upper_) {
    upper_ = upper();
    if (!upper_) {
      return std::nullopt;
    }
  }
  upper_.value() = evaluate::Fold(context, std::move(upper_.value().value()));
  std::optional<ConstantSubscript> ubi{ToInt64(upper_.value().value())};
  if (!ubi) {
    return std::nullopt;
  }
  if (!lower_) {
    lower_ = AsExpr(Constant<SubscriptInteger>{1});
  }
  lower_.value() = evaluate::Fold(context, std::move(lower_.value().value()));
  std::optional<ConstantSubscript> lbi{ToInt64(lower_.value().value())};
  if (!lbi) {
    return std::nullopt;
  }
  if (*lbi > *ubi) { // empty result; canonicalize
    *lbi = 1;
    *ubi = 0;
    lower_ = AsExpr(Constant<SubscriptInteger>{*lbi});
    upper_ = AsExpr(Constant<SubscriptInteger>{*ubi});
  }
  std::optional<ConstantSubscript> length;
  std::optional<Expr<SomeCharacter>> strings; // a Constant<Character>
  if (const auto *literal{std::get_if<StaticDataObject::Pointer>(&parent_)}) {
    length = (*literal)->data().size();
    if (auto str{(*literal)->AsString()}) {
      strings =
          Expr<SomeCharacter>(Expr<Ascii>(Constant<Ascii>{std::move(*str)}));
    }
  } else if (const auto *dataRef{std::get_if<DataRef>(&parent_)}) {
    if (auto expr{AsGenericExpr(DataRef{*dataRef})}) {
      auto folded{evaluate::Fold(context, std::move(*expr))};
      if (IsActuallyConstant(folded)) {
        if (const auto *value{UnwrapExpr<Expr<SomeCharacter>>(folded)}) {
          strings = *value;
        }
      }
    }
  }
  std::optional<Expr<SomeCharacter>> result;
  if (strings) {
    result = common::visit(
        [&](const auto &expr) -> std::optional<Expr<SomeCharacter>> {
          using Type = typename std::decay_t<decltype(expr)>::Result;
          if (const auto *cc{std::get_if<Constant<Type>>(&expr.u)}) {
            if (auto substr{cc->Substring(*lbi, *ubi)}) {
              return Expr<SomeCharacter>{Expr<Type>{*substr}};
            }
          }
          return std::nullopt;
        },
        strings->u);
  }
  if (!result) { // error cases
    if (*lbi < 1) {
      if (context.languageFeatures().ShouldWarn(common::UsageWarning::Bounds)) {
        context.messages().Say(common::UsageWarning::Bounds,
            "Lower bound (%jd) on substring is less than one"_warn_en_US,
            static_cast<std::intmax_t>(*lbi));
      }
      *lbi = 1;
      lower_ = AsExpr(Constant<SubscriptInteger>{1});
    }
    if (length && *ubi > *length) {
      if (context.languageFeatures().ShouldWarn(common::UsageWarning::Bounds)) {
        context.messages().Say(common::UsageWarning::Bounds,
            "Upper bound (%jd) on substring is greater than character length (%jd)"_warn_en_US,
            static_cast<std::intmax_t>(*ubi),
            static_cast<std::intmax_t>(*length));
      }
      *ubi = *length;
      upper_ = AsExpr(Constant<SubscriptInteger>{*ubi});
    }
  }
  return result;
}

DescriptorInquiry::DescriptorInquiry(
    const NamedEntity &base, Field field, int dim)
    : base_{base}, field_{field}, dimension_{dim} {
  const Symbol &last{base_.GetLastSymbol()};
  CHECK(IsDescriptor(last));
  CHECK(((field == Field::Len || field == Field::Rank) && dim == 0) ||
      (field != Field::Len && dim >= 0 && dim < last.Rank()));
}

DescriptorInquiry::DescriptorInquiry(NamedEntity &&base, Field field, int dim)
    : base_{std::move(base)}, field_{field}, dimension_{dim} {
  const Symbol &last{base_.GetLastSymbol()};
  CHECK(IsDescriptor(last));
  CHECK((field == Field::Len && dim == 0) ||
      (field != Field::Len && dim >= 0 &&
          (dim < last.Rank() || IsAssumedRank(last))));
}

// LEN()
static std::optional<Expr<SubscriptInteger>> SymbolLEN(const Symbol &symbol) {
  const Symbol &ultimate{symbol.GetUltimate()};
  if (const auto *assoc{ultimate.detailsIf<semantics::AssocEntityDetails>()}) {
    if (const auto *chExpr{UnwrapExpr<Expr<SomeCharacter>>(assoc->expr())}) {
      return chExpr->LEN();
    }
  }
  if (auto dyType{DynamicType::From(ultimate)}) {
    auto len{dyType->GetCharLength()};
    if (!len && ultimate.attrs().test(semantics::Attr::PARAMETER)) {
      // Its initializer determines the length of an implied-length named
      // constant.
      if (const auto *object{
              ultimate.detailsIf<semantics::ObjectEntityDetails>()}) {
        if (object->init()) {
          if (auto dyType2{DynamicType::From(*object->init())}) {
            len = dyType2->GetCharLength();
          }
        }
      }
    }
    if (len) {
      if (auto constLen{ToInt64(*len)}) {
        return Expr<SubscriptInteger>{std::max<std::int64_t>(*constLen, 0)};
      } else if (ultimate.owner().IsDerivedType() ||
          IsScopeInvariantExpr(*len)) {
        return AsExpr(Extremum<SubscriptInteger>{
            Ordering::Greater, Expr<SubscriptInteger>{0}, std::move(*len)});
      }
    }
  }
  if (IsDescriptor(ultimate) && !ultimate.owner().IsDerivedType()) {
    return Expr<SubscriptInteger>{
        DescriptorInquiry{NamedEntity{symbol}, DescriptorInquiry::Field::Len}};
  }
  return std::nullopt;
}

std::optional<Expr<SubscriptInteger>> BaseObject::LEN() const {
  return common::visit(
      common::visitors{
          [](const Symbol &symbol) { return SymbolLEN(symbol); },
          [](const StaticDataObject::Pointer &object)
              -> std::optional<Expr<SubscriptInteger>> {
            return AsExpr(Constant<SubscriptInteger>{object->data().size()});
          },
      },
      u);
}

std::optional<Expr<SubscriptInteger>> Component::LEN() const {
  return SymbolLEN(GetLastSymbol());
}

std::optional<Expr<SubscriptInteger>> NamedEntity::LEN() const {
  return SymbolLEN(GetLastSymbol());
}

std::optional<Expr<SubscriptInteger>> ArrayRef::LEN() const {
  return base_.LEN();
}

std::optional<Expr<SubscriptInteger>> CoarrayRef::LEN() const {
  return SymbolLEN(GetLastSymbol());
}

std::optional<Expr<SubscriptInteger>> DataRef::LEN() const {
  return common::visit(common::visitors{
                           [](SymbolRef symbol) { return SymbolLEN(symbol); },
                           [](const auto &x) { return x.LEN(); },
                       },
      u);
}

std::optional<Expr<SubscriptInteger>> Substring::LEN() const {
  if (auto top{upper()}) {
    return AsExpr(Extremum<SubscriptInteger>{Ordering::Greater,
        AsExpr(Constant<SubscriptInteger>{0}),
        *std::move(top) - lower() + AsExpr(Constant<SubscriptInteger>{1})});
  } else {
    return std::nullopt;
  }
}

template <typename T>
std::optional<Expr<SubscriptInteger>> Designator<T>::LEN() const {
  if constexpr (T::category == TypeCategory::Character) {
    return common::visit(common::visitors{
                             [](SymbolRef symbol) { return SymbolLEN(symbol); },
                             [](const auto &x) { return x.LEN(); },
                         },
        u);
  } else {
    common::die("Designator<non-char>::LEN() called");
    return std::nullopt;
  }
}

std::optional<Expr<SubscriptInteger>> ProcedureDesignator::LEN() const {
  using T = std::optional<Expr<SubscriptInteger>>;
  return common::visit(
      common::visitors{
          [](SymbolRef symbol) -> T { return SymbolLEN(symbol); },
          [](const common::CopyableIndirection<Component> &c) -> T {
            return c.value().LEN();
          },
          [](const SpecificIntrinsic &i) -> T {
            // Some cases whose results' lengths can be determined
            // from the lengths of their arguments are handled in
            // ProcedureRef::LEN() before coming here.
            if (const auto &result{i.characteristics.value().functionResult}) {
              if (const auto *type{result->GetTypeAndShape()}) {
                if (auto length{type->type().GetCharLength()}) {
                  return std::move(*length);
                }
              }
            }
            return std::nullopt;
          },
      },
      u);
}

// Rank()
int BaseObject::Rank() const {
  return common::visit(common::visitors{
                           [](SymbolRef symbol) { return symbol->Rank(); },
                           [](const StaticDataObject::Pointer &) { return 0; },
                       },
      u);
}

int Component::Rank() const {
  if (int rank{symbol_->Rank()}; rank > 0) {
    return rank;
  }
  return base().Rank();
}

int NamedEntity::Rank() const {
  return common::visit(common::visitors{
                           [](const SymbolRef s) { return s->Rank(); },
                           [](const Component &c) { return c.Rank(); },
                       },
      u_);
}

int Subscript::Rank() const {
  return common::visit(common::visitors{
                           [](const IndirectSubscriptIntegerExpr &x) {
                             return x.value().Rank();
                           },
                           [](const Triplet &) { return 1; },
                       },
      u);
}

int ArrayRef::Rank() const {
  int rank{0};
  for (const auto &expr : subscript_) {
    rank += expr.Rank();
  }
  if (rank > 0) {
    return rank;
  } else if (const Component * component{base_.UnwrapComponent()}) {
    return component->base().Rank();
  } else {
    return 0;
  }
}

int CoarrayRef::Rank() const { return base().Rank(); }

int DataRef::Rank() const {
  return common::visit(common::visitors{
                           [](SymbolRef symbol) { return symbol->Rank(); },
                           [](const auto &x) { return x.Rank(); },
                       },
      u);
}

int Substring::Rank() const {
  return common::visit(
      common::visitors{
          [](const DataRef &dataRef) { return dataRef.Rank(); },
          [](const StaticDataObject::Pointer &) { return 0; },
      },
      parent_);
}

int ComplexPart::Rank() const { return complex_.Rank(); }

template <typename T> int Designator<T>::Rank() const {
  return common::visit(common::visitors{
                           [](SymbolRef symbol) { return symbol->Rank(); },
                           [](const auto &x) { return x.Rank(); },
                       },
      u);
}

// Corank()
int BaseObject::Corank() const {
  return common::visit(common::visitors{
                           [](SymbolRef symbol) { return symbol->Corank(); },
                           [](const StaticDataObject::Pointer &) { return 0; },
                       },
      u);
}

int Component::Corank() const {
  if (int corank{symbol_->Corank()}; corank > 0) {
    return corank;
  } else if (semantics::IsAllocatableOrObjectPointer(&*symbol_)) {
    return 0; // coarray subobjects ca%a or ca%p are not coarrays
  } else {
    return base().Corank();
  }
}

int NamedEntity::Corank() const {
  return common::visit(common::visitors{
                           [](const SymbolRef s) { return s->Corank(); },
                           [](const Component &c) { return c.Corank(); },
                       },
      u_);
}

int ArrayRef::Corank() const {
  for (const Subscript &subs : subscript_) {
    if (!std::holds_alternative<Triplet>(subs.u) && subs.Rank() > 0) {
      return 0; // vector-valued subscript - subobject is not a coarray
    }
  }
  return base().Corank();
}

int DataRef::Corank() const {
  return common::visit(common::visitors{
                           [](SymbolRef symbol) { return symbol->Corank(); },
                           [](const auto &x) { return x.Corank(); },
                       },
      u);
}

int Substring::Corank() const {
  return common::visit(
      common::visitors{
          [](const DataRef &dataRef) { return dataRef.Corank(); },
          [](const StaticDataObject::Pointer &) { return 0; },
      },
      parent_);
}

int ComplexPart::Corank() const { return complex_.Corank(); }

template <typename T> int Designator<T>::Corank() const {
  return common::visit(common::visitors{
                           [](SymbolRef symbol) { return symbol->Corank(); },
                           [](const auto &x) { return x.Corank(); },
                       },
      u);
}

// GetBaseObject(), GetFirstSymbol(), GetLastSymbol(), &c.
const Symbol &Component::GetFirstSymbol() const {
  return base_.value().GetFirstSymbol();
}

const Symbol &NamedEntity::GetFirstSymbol() const {
  return common::visit(common::visitors{
                           [](SymbolRef s) -> const Symbol & { return s; },
                           [](const Component &c) -> const Symbol & {
                             return c.GetFirstSymbol();
                           },
                       },
      u_);
}

const Symbol &NamedEntity::GetLastSymbol() const {
  return common::visit(common::visitors{
                           [](SymbolRef s) -> const Symbol & { return s; },
                           [](const Component &c) -> const Symbol & {
                             return c.GetLastSymbol();
                           },
                       },
      u_);
}

const SymbolRef *NamedEntity::UnwrapSymbolRef() const {
  return common::visit(
      common::visitors{
          [](const SymbolRef &s) { return &s; },
          [](const Component &) -> const SymbolRef * { return nullptr; },
      },
      u_);
}

SymbolRef *NamedEntity::UnwrapSymbolRef() {
  return common::visit(common::visitors{
                           [](SymbolRef &s) { return &s; },
                           [](Component &) -> SymbolRef * { return nullptr; },
                       },
      u_);
}

const Component *NamedEntity::UnwrapComponent() const {
  return common::visit(
      common::visitors{
          [](SymbolRef) -> const Component * { return nullptr; },
          [](const Component &c) { return &c; },
      },
      u_);
}

Component *NamedEntity::UnwrapComponent() {
  return common::visit(common::visitors{
                           [](SymbolRef &) -> Component * { return nullptr; },
                           [](Component &c) { return &c; },
                       },
      u_);
}

const Symbol &ArrayRef::GetFirstSymbol() const {
  return base_.GetFirstSymbol();
}

const Symbol &ArrayRef::GetLastSymbol() const { return base_.GetLastSymbol(); }

const Symbol &DataRef::GetFirstSymbol() const {
  return *common::visit(common::visitors{
                            [](SymbolRef symbol) { return &*symbol; },
                            [](const auto &x) { return &x.GetFirstSymbol(); },
                        },
      u);
}

const Symbol &DataRef::GetLastSymbol() const {
  return *common::visit(common::visitors{
                            [](SymbolRef symbol) { return &*symbol; },
                            [](const auto &x) { return &x.GetLastSymbol(); },
                        },
      u);
}

BaseObject Substring::GetBaseObject() const {
  return common::visit(common::visitors{
                           [](const DataRef &dataRef) {
                             return BaseObject{dataRef.GetFirstSymbol()};
                           },
                           [](StaticDataObject::Pointer pointer) {
                             return BaseObject{std::move(pointer)};
                           },
                       },
      parent_);
}

const Symbol *Substring::GetLastSymbol() const {
  return common::visit(
      common::visitors{
          [](const DataRef &dataRef) { return &dataRef.GetLastSymbol(); },
          [](const auto &) -> const Symbol * { return nullptr; },
      },
      parent_);
}

template <typename T> BaseObject Designator<T>::GetBaseObject() const {
  return common::visit(
      common::visitors{
          [](SymbolRef symbol) { return BaseObject{symbol}; },
          [](const Substring &sstring) { return sstring.GetBaseObject(); },
          [](const auto &x) { return BaseObject{x.GetFirstSymbol()}; },
      },
      u);
}

template <typename T> const Symbol *Designator<T>::GetLastSymbol() const {
  return common::visit(
      common::visitors{
          [](SymbolRef symbol) { return &*symbol; },
          [](const Substring &sstring) { return sstring.GetLastSymbol(); },
          [](const auto &x) { return &x.GetLastSymbol(); },
      },
      u);
}

template <typename T>
std::optional<DynamicType> Designator<T>::GetType() const {
  if constexpr (IsLengthlessIntrinsicType<Result>) {
    return Result::GetType();
  }
  if constexpr (Result::category == TypeCategory::Character) {
    if (std::holds_alternative<Substring>(u)) {
      if (auto len{LEN()}) {
        if (auto n{ToInt64(*len)}) {
          return DynamicType{T::kind, *n};
        }
      }
      return DynamicType{TypeCategory::Character, T::kind};
    }
  }
  if (const Symbol * symbol{GetLastSymbol()}) {
    return DynamicType::From(*symbol);
  }
  return std::nullopt;
}

// Equality testing

// For the purposes of comparing type parameter expressions while
// testing the compatibility of procedure characteristics, two
// dummy arguments with the same position are considered equal.
static std::optional<int> GetDummyArgPosition(const Symbol &original) {
  const Symbol &symbol(original.GetUltimate());
  if (IsDummy(symbol)) {
    if (const Symbol * proc{symbol.owner().symbol()}) {
      if (const auto *subp{proc->detailsIf<semantics::SubprogramDetails>()}) {
        int j{0};
        for (const Symbol *arg : subp->dummyArgs()) {
          if (arg == &symbol) {
            return j;
          }
          ++j;
        }
      }
    }
  }
  return std::nullopt;
}

static bool AreSameSymbol(const Symbol &x, const Symbol &y) {
  if (&x == &y) {
    return true;
  }
  if (auto xPos{GetDummyArgPosition(x)}) {
    if (auto yPos{GetDummyArgPosition(y)}) {
      return *xPos == *yPos;
    }
  }
  return false;
}

// Implements operator==() for a union type, using special case handling
// for Symbol references.
template <typename A> static bool TestVariableEquality(const A &x, const A &y) {
  const SymbolRef *xSymbol{std::get_if<SymbolRef>(&x.u)};
  if (const SymbolRef * ySymbol{std::get_if<SymbolRef>(&y.u)}) {
    return xSymbol && AreSameSymbol(*xSymbol, *ySymbol);
  } else {
    return x.u == y.u;
  }
}

bool BaseObject::operator==(const BaseObject &that) const {
  return TestVariableEquality(*this, that);
}
bool Component::operator==(const Component &that) const {
  return base_ == that.base_ && &*symbol_ == &*that.symbol_;
}
bool NamedEntity::operator==(const NamedEntity &that) const {
  if (IsSymbol()) {
    return that.IsSymbol() &&
        AreSameSymbol(GetFirstSymbol(), that.GetFirstSymbol());
  } else {
    return !that.IsSymbol() && GetComponent() == that.GetComponent();
  }
}
bool TypeParamInquiry::operator==(const TypeParamInquiry &that) const {
  return &*parameter_ == &*that.parameter_ && base_ == that.base_;
}
bool Triplet::operator==(const Triplet &that) const {
  return lower_ == that.lower_ && upper_ == that.upper_ &&
      stride_ == that.stride_;
}
bool Subscript::operator==(const Subscript &that) const { return u == that.u; }
bool ArrayRef::operator==(const ArrayRef &that) const {
  return base_ == that.base_ && subscript_ == that.subscript_;
}
bool CoarrayRef::operator==(const CoarrayRef &that) const {
  return base_ == that.base_ && cosubscript_ == that.cosubscript_ &&
      stat_ == that.stat_ && team_ == that.team_;
}
bool DataRef::operator==(const DataRef &that) const {
  return TestVariableEquality(*this, that);
}
bool Substring::operator==(const Substring &that) const {
  return parent_ == that.parent_ && lower_ == that.lower_ &&
      upper_ == that.upper_;
}
bool ComplexPart::operator==(const ComplexPart &that) const {
  return part_ == that.part_ && complex_ == that.complex_;
}
bool ProcedureRef::operator==(const ProcedureRef &that) const {
  return proc_ == that.proc_ && arguments_ == that.arguments_;
}
template <typename T>
bool Designator<T>::operator==(const Designator<T> &that) const {
  return TestVariableEquality(*this, that);
}
bool DescriptorInquiry::operator==(const DescriptorInquiry &that) const {
  return field_ == that.field_ && base_ == that.base_ &&
      dimension_ == that.dimension_;
}

#ifdef _MSC_VER // disable bogus warning about missing definitions
#pragma warning(disable : 4661)
#endif
INSTANTIATE_VARIABLE_TEMPLATES
} // namespace Fortran::evaluate

template class Fortran::common::Indirection<Fortran::evaluate::Component, true>;
