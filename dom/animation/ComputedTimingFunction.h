/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ComputedTimingFunction_h
#define mozilla_ComputedTimingFunction_h

#include "nsSMILKeySpline.h"  // nsSMILKeySpline
#include "nsStyleStruct.h"    // nsTimingFunction

namespace mozilla {

class ComputedTimingFunction
{
public:
  void Init(const nsTimingFunction &aFunction);
  double GetValue(double aPortion) const;
  const nsSMILKeySpline* GetFunction() const
  {
    NS_ASSERTION(HasSpline(), "Type mismatch");
    return &mTimingFunction;
  }
  nsTimingFunction::Type GetType() const { return mType; }
  bool HasSpline() const { return nsTimingFunction::IsSplineType(mType); }
  uint32_t GetSteps() const { return mSteps; }
  nsTimingFunction::StepSyntax GetStepSyntax() const { return mStepSyntax; }
  bool operator==(const ComputedTimingFunction& aOther) const
  {
    return mType == aOther.mType &&
           (HasSpline() ?
            mTimingFunction == aOther.mTimingFunction :
            (mSteps == aOther.mSteps &&
             mStepSyntax == aOther.mStepSyntax));
  }
  bool operator!=(const ComputedTimingFunction& aOther) const
  {
    return !(*this == aOther);
  }
  int32_t Compare(const ComputedTimingFunction& aRhs) const;
  void AppendToString(nsAString& aResult) const;

private:
  nsTimingFunction::Type mType;
  nsSMILKeySpline mTimingFunction;
  uint32_t mSteps;
  nsTimingFunction::StepSyntax mStepSyntax;
};

} // namespace mozilla

#endif // mozilla_ComputedTimingFunction_h
