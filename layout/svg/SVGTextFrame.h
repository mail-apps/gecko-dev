/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_SVGTEXTFRAME_H
#define MOZILLA_SVGTEXTFRAME_H

#include "mozilla/Attributes.h"
#include "mozilla/RefPtr.h"
#include "mozilla/gfx/2D.h"
#include "gfxMatrix.h"
#include "gfxRect.h"
#include "gfxSVGGlyphs.h"
#include "nsIContent.h" // for GetContent
#include "nsStubMutationObserver.h"
#include "nsSVGPaintServerFrame.h"

class gfxContext;
class nsDisplaySVGText;
class SVGTextFrame;
class nsTextFrame;

typedef nsSVGDisplayContainerFrame SVGTextFrameBase;

namespace mozilla {

class CharIterator;
class nsISVGPoint;
class TextFrameIterator;
class TextNodeCorrespondenceRecorder;
struct TextRenderedRun;
class TextRenderedRunIterator;

namespace dom {
class SVGIRect;
class SVGPathElement;
} // namespace dom

/**
 * Information about the positioning for a single character in an SVG <text>
 * element.
 *
 * During SVG text layout, we use infinity values to represent positions and
 * rotations that are not explicitly specified with x/y/rotate attributes.
 */
struct CharPosition
{
  CharPosition()
    : mAngle(0),
      mHidden(false),
      mUnaddressable(false),
      mClusterOrLigatureGroupMiddle(false),
      mRunBoundary(false),
      mStartOfChunk(false)
  {
  }

  CharPosition(gfxPoint aPosition, double aAngle)
    : mPosition(aPosition),
      mAngle(aAngle),
      mHidden(false),
      mUnaddressable(false),
      mClusterOrLigatureGroupMiddle(false),
      mRunBoundary(false),
      mStartOfChunk(false)
  {
  }

  static CharPosition Unspecified(bool aUnaddressable)
  {
    CharPosition cp(UnspecifiedPoint(), UnspecifiedAngle());
    cp.mUnaddressable = aUnaddressable;
    return cp;
  }

  bool IsAngleSpecified() const
  {
    return mAngle != UnspecifiedAngle();
  }

  bool IsXSpecified() const
  {
    return mPosition.x != UnspecifiedCoord();
  }

  bool IsYSpecified() const
  {
    return mPosition.y != UnspecifiedCoord();
  }

  gfxPoint mPosition;
  double mAngle;

  // not displayed due to falling off the end of a <textPath>
  bool mHidden;

  // skipped in positioning attributes due to being collapsed-away white space
  bool mUnaddressable;

  // a preceding character is what positioning attributes address
  bool mClusterOrLigatureGroupMiddle;

  // rendering is split here since an explicit position or rotation was given
  bool mRunBoundary;

  // an anchored chunk begins here
  bool mStartOfChunk;

private:
  static gfxFloat UnspecifiedCoord()
  {
    return std::numeric_limits<gfxFloat>::infinity();
  }

  static double UnspecifiedAngle()
  {
    return std::numeric_limits<double>::infinity();
  }

  static gfxPoint UnspecifiedPoint()
  {
    return gfxPoint(UnspecifiedCoord(), UnspecifiedCoord());
  }
};

/**
 * A runnable to mark glyph positions as needing to be recomputed
 * and to invalid the bounds of the SVGTextFrame frame.
 */
class GlyphMetricsUpdater : public nsRunnable {
public:
  NS_DECL_NSIRUNNABLE
  explicit GlyphMetricsUpdater(SVGTextFrame* aFrame) : mFrame(aFrame) { }
  static void Run(SVGTextFrame* aFrame);
  void Revoke() { mFrame = nullptr; }
private:
  SVGTextFrame* mFrame;
};

// Slightly horrible callback for deferring application of opacity
struct SVGTextContextPaint : public gfxTextContextPaint {
protected:
  typedef mozilla::gfx::DrawTarget DrawTarget;
public:
  already_AddRefed<gfxPattern> GetFillPattern(const DrawTarget* aDrawTarget,
                                              float aOpacity,
                                              const gfxMatrix& aCTM) override;
  already_AddRefed<gfxPattern> GetStrokePattern(const DrawTarget* aDrawTarget,
                                                float aOpacity,
                                                const gfxMatrix& aCTM) override;

  void SetFillOpacity(float aOpacity) { mFillOpacity = aOpacity; }
  float GetFillOpacity() override { return mFillOpacity; }

  void SetStrokeOpacity(float aOpacity) { mStrokeOpacity = aOpacity; }
  float GetStrokeOpacity() override { return mStrokeOpacity; }

  struct Paint {
    Paint() : mPaintType(eStyleSVGPaintType_None) {}

    void SetPaintServer(nsIFrame *aFrame, const gfxMatrix& aContextMatrix,
                        nsSVGPaintServerFrame *aPaintServerFrame) {
      mPaintType = eStyleSVGPaintType_Server;
      mPaintDefinition.mPaintServerFrame = aPaintServerFrame;
      mFrame = aFrame;
      mContextMatrix = aContextMatrix;
    }

    void SetColor(const nscolor &aColor) {
      mPaintType = eStyleSVGPaintType_Color;
      mPaintDefinition.mColor = aColor;
    }

    void SetContextPaint(gfxTextContextPaint *aContextPaint,
                         nsStyleSVGPaintType aPaintType) {
      NS_ASSERTION(aPaintType == eStyleSVGPaintType_ContextFill ||
                   aPaintType == eStyleSVGPaintType_ContextStroke,
                   "Invalid context paint type");
      mPaintType = aPaintType;
      mPaintDefinition.mContextPaint = aContextPaint;
    }

    union {
      nsSVGPaintServerFrame *mPaintServerFrame;
      gfxTextContextPaint *mContextPaint;
      nscolor mColor;
    } mPaintDefinition;

    nsIFrame *mFrame;
    // CTM defining the user space for the pattern we will use.
    gfxMatrix mContextMatrix;
    nsStyleSVGPaintType mPaintType;

    // Device-space-to-pattern-space
    gfxMatrix mPatternMatrix;
    nsRefPtrHashtable<nsFloatHashKey, gfxPattern> mPatternCache;

    already_AddRefed<gfxPattern> GetPattern(const DrawTarget* aDrawTarget,
                                            float aOpacity,
                                            nsStyleSVGPaint nsStyleSVG::*aFillOrStroke,
                                            const gfxMatrix& aCTM);
  };

  Paint mFillPaint;
  Paint mStrokePaint;

  float mFillOpacity;
  float mStrokeOpacity;
};

} // namespace mozilla

/**
 * Frame class for SVG <text> elements.
 *
 * An SVGTextFrame manages SVG text layout, painting and interaction for
 * all descendent text content elements.  The frame tree will look like this:
 *
 *   SVGTextFrame                     -- for <text>
 *     <anonymous block frame>
 *       ns{Block,Inline,Text}Frames  -- for text nodes, <tspan>s, <a>s, etc.
 *
 * SVG text layout is done by:
 *
 *   1. Reflowing the anonymous block frame.
 *   2. Inspecting the (app unit) positions of the glyph for each character in
 *      the nsTextFrames underneath the anonymous block frame.
 *   3. Determining the (user unit) positions for each character in the <text>
 *      using the x/y/dx/dy/rotate attributes on all the text content elements,
 *      and using the step 2 results to fill in any gaps.
 *   4. Applying any other SVG specific text layout (anchoring and text paths)
 *      to the positions computed in step 3.
 *
 * Rendering of the text is done by splitting up each nsTextFrame into ranges
 * that can be contiguously painted.  (For example <text x="10 20">abcd</text>
 * would have two contiguous ranges: one for the "a" and one for the "bcd".)
 * Each range is called a "text rendered run", represented by a TextRenderedRun
 * object.  The TextRenderedRunIterator class performs that splitting and
 * returns a TextRenderedRun for each bit of text to be painted separately.
 *
 * Each rendered run is painted by calling nsTextFrame::PaintText.  If the text
 * formatting is simple enough (solid fill, no stroking, etc.), PaintText will
 * itself do the painting.  Otherwise, a DrawPathCallback is passed to
 * PaintText so that we can fill the text geometry with SVG paint servers.
 */
class SVGTextFrame final : public SVGTextFrameBase
{
  friend nsIFrame*
  NS_NewSVGTextFrame(nsIPresShell* aPresShell, nsStyleContext* aContext);

  friend class mozilla::CharIterator;
  friend class mozilla::GlyphMetricsUpdater;
  friend class mozilla::TextFrameIterator;
  friend class mozilla::TextNodeCorrespondenceRecorder;
  friend struct mozilla::TextRenderedRun;
  friend class mozilla::TextRenderedRunIterator;
  friend class MutationObserver;
  friend class nsDisplaySVGText;

  typedef mozilla::gfx::DrawTarget DrawTarget;
  typedef mozilla::gfx::Path Path;
  typedef mozilla::gfx::Point Point;
  typedef mozilla::SVGTextContextPaint SVGTextContextPaint;

protected:
  explicit SVGTextFrame(nsStyleContext* aContext)
    : SVGTextFrameBase(aContext),
      mFontSizeScaleFactor(1.0f),
      mLastContextScale(1.0f),
      mLengthAdjustScaleFactor(1.0f)
  {
    AddStateBits(NS_STATE_SVG_POSITIONING_DIRTY);
  }

  ~SVGTextFrame() {}

public:
  NS_DECL_QUERYFRAME_TARGET(SVGTextFrame)
  NS_DECL_QUERYFRAME
  NS_DECL_FRAMEARENA_HELPERS

  // nsIFrame:
  virtual void Init(nsIContent*       aContent,
                    nsContainerFrame* aParent,
                    nsIFrame*         aPrevInFlow) override;

  virtual nsresult AttributeChanged(int32_t aNamespaceID,
                                    nsIAtom* aAttribute,
                                    int32_t aModType) override;

  virtual nsContainerFrame* GetContentInsertionFrame() override
  {
    return GetFirstPrincipalChild()->GetContentInsertionFrame();
  }

  virtual void BuildDisplayList(nsDisplayListBuilder*   aBuilder,
                                const nsRect&           aDirtyRect,
                                const nsDisplayListSet& aLists) override;

  /**
   * Get the "type" of the frame
   *
   * @see nsGkAtoms::svgTextFrame
   */
  virtual nsIAtom* GetType() const override;

#ifdef DEBUG_FRAME_DUMP
  virtual nsresult GetFrameName(nsAString& aResult) const override
  {
    return MakeFrameName(NS_LITERAL_STRING("SVGText"), aResult);
  }
#endif

  virtual void DidSetStyleContext(nsStyleContext* aOldStyleContext) override;

  /**
   * Finds the nsTextFrame for the closest rendered run to the specified point.
   */
  virtual void FindCloserFrameForSelection(nsPoint aPoint,
                                          FrameWithDistance* aCurrentBestFrame) override;



  // nsISVGChildFrame interface:
  virtual void NotifySVGChanged(uint32_t aFlags) override;
  virtual nsresult PaintSVG(gfxContext& aContext,
                            const gfxMatrix& aTransform,
                            const nsIntRect* aDirtyRect = nullptr) override;
  virtual nsIFrame* GetFrameForPoint(const gfxPoint& aPoint) override;
  virtual void ReflowSVG() override;
  virtual nsRect GetCoveredRegion() override;
  virtual SVGBBox GetBBoxContribution(const Matrix& aToBBoxUserspace,
                                      uint32_t aFlags) override;

  // nsSVGContainerFrame methods:
  virtual gfxMatrix GetCanvasTM() override;
  
  // SVG DOM text methods:
  uint32_t GetNumberOfChars(nsIContent* aContent);
  float GetComputedTextLength(nsIContent* aContent);
  nsresult SelectSubString(nsIContent* aContent, uint32_t charnum, uint32_t nchars);
  nsresult GetSubStringLength(nsIContent* aContent, uint32_t charnum,
                              uint32_t nchars, float* aResult);
  int32_t GetCharNumAtPosition(nsIContent* aContent, mozilla::nsISVGPoint* point);

  nsresult GetStartPositionOfChar(nsIContent* aContent, uint32_t aCharNum,
                                  mozilla::nsISVGPoint** aResult);
  nsresult GetEndPositionOfChar(nsIContent* aContent, uint32_t aCharNum,
                                mozilla::nsISVGPoint** aResult);
  nsresult GetExtentOfChar(nsIContent* aContent, uint32_t aCharNum,
                           mozilla::dom::SVGIRect** aResult);
  nsresult GetRotationOfChar(nsIContent* aContent, uint32_t aCharNum,
                             float* aResult);

  // SVGTextFrame methods:

  /**
   * Handles a base or animated attribute value change to a descendant
   * text content element.
   */
  void HandleAttributeChangeInDescendant(mozilla::dom::Element* aElement,
                                         int32_t aNameSpaceID,
                                         nsIAtom* aAttribute);

  /**
   * Schedules mPositions to be recomputed and the covered region to be
   * updated.
   */
  void NotifyGlyphMetricsChange();

  /**
   * Calls ScheduleReflowSVGNonDisplayText if this is a non-display frame,
   * and nsSVGUtils::ScheduleReflowSVG otherwise.
   */
  void ScheduleReflowSVG();

  /**
   * Reflows the anonymous block frame of this non-display SVGTextFrame.
   *
   * When we are under nsSVGDisplayContainerFrame::ReflowSVG, we need to
   * reflow any SVGTextFrame frames in the subtree in case they are
   * being observed (by being for example in a <mask>) and the change
   * that caused the reflow would not already have caused a reflow.
   *
   * Note that displayed SVGTextFrames are reflowed as needed, when PaintSVG
   * is called or some SVG DOM method is called on the element.
   */
  void ReflowSVGNonDisplayText();

  /**
   * This is a function that behaves similarly to nsSVGUtils::ScheduleReflowSVG,
   * but which will skip over any ancestor non-display container frames on the
   * way to the nsSVGOuterSVGFrame.  It exists for the situation where a
   * non-display <text> element has changed and needs to ensure ReflowSVG will
   * be called on its closest display container frame, so that
   * nsSVGDisplayContainerFrame::ReflowSVG will call ReflowSVGNonDisplayText on
   * it.
   *
   * The only case where we have to do this is in response to a style change on
   * a non-display <text>; the only caller of ScheduleReflowSVGNonDisplayText
   * currently is SVGTextFrame::DidSetStyleContext.
   */
  void ScheduleReflowSVGNonDisplayText();

  /**
   * Updates the mFontSizeScaleFactor value by looking at the range of
   * font-sizes used within the <text>.
   *
   * @return Whether mFontSizeScaleFactor changed.
   */
  bool UpdateFontSizeScaleFactor();

  double GetFontSizeScaleFactor() const;

  /**
   * Takes a point from the <text> element's user space and
   * converts it to the appropriate frame user space of aChildFrame,
   * according to which rendered run the point hits.
   */
  Point TransformFramePointToTextChild(const Point& aPoint,
                                       nsIFrame* aChildFrame);

  /**
   * Takes a rectangle, aRect, in the <text> element's user space, and
   * returns a rectangle in aChildFrame's frame user space that
   * covers intersections of aRect with each rendered run for text frames
   * within aChildFrame.
   */
  gfxRect TransformFrameRectToTextChild(const gfxRect& aRect,
                                        nsIFrame* aChildFrame);

  /**
   * Takes an app unit rectangle in the coordinate space of a given descendant
   * frame of this frame, and returns a rectangle in the <text> element's user
   * space that covers all parts of rendered runs that intersect with the
   * rectangle.
   */
  gfxRect TransformFrameRectFromTextChild(const nsRect& aRect,
                                          nsIFrame* aChildFrame);

private:
  /**
   * Mutation observer used to watch for text positioning attribute changes
   * on descendent text content elements (like <tspan>s).
   */
  class MutationObserver final : public nsStubMutationObserver {
  public:
    explicit MutationObserver(SVGTextFrame* aFrame)
      : mFrame(aFrame)
    {
      MOZ_ASSERT(mFrame, "MutationObserver needs a non-null frame");
      mFrame->GetContent()->AddMutationObserver(this);
    }

    // nsISupports
    NS_DECL_ISUPPORTS

    // nsIMutationObserver
    NS_DECL_NSIMUTATIONOBSERVER_CONTENTAPPENDED
    NS_DECL_NSIMUTATIONOBSERVER_CONTENTINSERTED
    NS_DECL_NSIMUTATIONOBSERVER_CONTENTREMOVED
    NS_DECL_NSIMUTATIONOBSERVER_CHARACTERDATACHANGED
    NS_DECL_NSIMUTATIONOBSERVER_ATTRIBUTECHANGED

  private:
    ~MutationObserver()
    {
      mFrame->GetContent()->RemoveMutationObserver(this);
    }

    SVGTextFrame* const mFrame;
  };

  /**
   * Reflows the anonymous block child if it is dirty or has dirty
   * children, or if the SVGTextFrame itself is dirty.
   */
  void MaybeReflowAnonymousBlockChild();

  /**
   * Performs the actual work of reflowing the anonymous block child.
   */
  void DoReflow();

  /**
   * Recomputes mPositions by calling DoGlyphPositioning if this information
   * is out of date.
   */
  void UpdateGlyphPositioning();

  /**
   * Populates mPositions with positioning information for each character
   * within the <text>.
   */
  void DoGlyphPositioning();

  /**
   * Converts the specified index into mPositions to an addressable
   * character index (as can be used with the SVG DOM text methods)
   * relative to the specified text child content element.
   *
   * @param aIndex The global character index.
   * @param aContent The descendant text child content element that
   *   the returned addressable index will be relative to; null
   *   means the same as the <text> element.
   * @return The addressable index, or -1 if the index cannot be
   *   represented as an addressable index relative to aContent.
   */
  int32_t
  ConvertTextElementCharIndexToAddressableIndex(int32_t aIndex,
                                                nsIContent* aContent);

  /**
   * Recursive helper for ResolvePositions below.
   *
   * @param aContent The current node.
   * @param aIndex (in/out) The current character index.
   * @param aInTextPath Whether we are currently under a <textPath> element.
   * @param aForceStartOfChunk (in/out) Whether the next character we find
   *   should start a new anchored chunk.
   * @param aDeltas (in/out) Receives the resolved dx/dy values for each
   *   character.
   * @return false if we discover that mPositions did not have enough
   *   elements; true otherwise.
   */
  bool ResolvePositionsForNode(nsIContent* aContent, uint32_t& aIndex,
                               bool aInTextPath, bool& aForceStartOfChunk,
                               nsTArray<gfxPoint>& aDeltas);

  /**
   * Initializes mPositions with character position information based on
   * x/y/rotate attributes, leaving unspecified values in the array if a position
   * was not given for that character.  Also fills aDeltas with values based on
   * dx/dy attributes.
   *
   * @param aDeltas (in/out) Receives the resolved dx/dy values for each
   *   character.
   * @param aRunPerGlyph Whether mPositions should record that a new run begins
   *   at each glyph.
   * @return false if we did not record any positions (due to having no
   *   displayed characters) or if we discover that mPositions did not have
   *   enough elements; true otherwise.
   */
  bool ResolvePositions(nsTArray<gfxPoint>& aDeltas, bool aRunPerGlyph);

  /**
   * Determines the position, in app units, of each character in the <text> as
   * laid out by reflow, and appends them to aPositions.  Any characters that
   * are undisplayed or trimmed away just get the last position.
   */
  void DetermineCharPositions(nsTArray<nsPoint>& aPositions);

  /**
   * Sets mStartOfChunk to true for each character in mPositions that starts a
   * line of text.
   */
  void AdjustChunksForLineBreaks();

  /**
   * Adjusts recorded character positions in mPositions to account for glyph
   * boundaries.  Four things are done:
   *
   *   1. mClusterOrLigatureGroupMiddle is set to true for all such characters.
   *
   *   2. Any run and anchored chunk boundaries that begin in the middle of a
   *      cluster/ligature group get moved to the start of the next
   *      cluster/ligature group.
   *
   *   3. The position of any character in the middle of a cluster/ligature
   *      group is updated to take into account partial ligatures and any
   *      rotation the glyph as a whole has.  (The values that come out of
   *      DetermineCharPositions which then get written into mPositions in
   *      ResolvePositions store the same position value for each part of the
   *      ligature.)
   *
   *   4. The rotation of any character in the middle of a cluster/ligature
   *      group is set to the rotation of the first character.
   */
  void AdjustPositionsForClusters();

  /**
   * Updates the character positions stored in mPositions to account for
   * text anchoring.
   */
  void DoAnchoring();

  /**
   * Updates character positions in mPositions for those characters inside a
   * <textPath>.
   */
  void DoTextPathLayout();

  /**
   * Returns whether we need to render the text using
   * nsTextFrame::DrawPathCallbacks rather than directly painting
   * the text frames.
   *
   * @param aShouldPaintSVGGlyphs (out) Whether SVG glyphs in the text
   *   should be painted.
   */
  bool ShouldRenderAsPath(nsTextFrame* aFrame, bool& aShouldPaintSVGGlyphs);

  // Methods to get information for a <textPath> frame.
  mozilla::dom::SVGPathElement*
  GetTextPathPathElement(nsIFrame* aTextPathFrame);
  already_AddRefed<Path> GetTextPath(nsIFrame* aTextPathFrame);
  gfxFloat GetOffsetScale(nsIFrame* aTextPathFrame);
  gfxFloat GetStartOffset(nsIFrame* aTextPathFrame);

  DrawMode SetupContextPaint(const DrawTarget* aDrawTarget,
                             const gfxMatrix& aContextMatrix,
                             nsIFrame* aFrame,
                             gfxTextContextPaint* aOuterContextPaint,
                             SVGTextContextPaint* aThisContextPaint);

  /**
   * The MutationObserver we have registered for the <text> element subtree.
   */
  RefPtr<MutationObserver> mMutationObserver;

  /**
   * Cached canvasTM value.
   */
  nsAutoPtr<gfxMatrix> mCanvasTM;

  /**
   * The number of characters in the DOM after the final nsTextFrame.  For
   * example, with
   *
   *   <text>abcd<tspan display="none">ef</tspan></text>
   *
   * mTrailingUndisplayedCharacters would be 2.
   */
  uint32_t mTrailingUndisplayedCharacters;

  /**
   * Computed position information for each DOM character within the <text>.
   */
  nsTArray<mozilla::CharPosition> mPositions;

  /**
   * mFontSizeScaleFactor is used to cause the nsTextFrames to create text
   * runs with a font size different from the actual font-size property value.
   * This is used so that, for example with:
   *
   *   <svg>
   *     <g transform="scale(2)">
   *       <text font-size="10">abc</text>
   *     </g>
   *   </svg>
   *
   * a font size of 20 would be used.  It's preferable to use a font size that
   * is identical or close to the size that the text will appear on the screen,
   * because at very small or large font sizes, text metrics will be computed
   * differently due to the limited precision that text runs have.
   *
   * mFontSizeScaleFactor is the amount the actual font-size property value
   * should be multiplied by to cause the text run font size to (a) be within a
   * "reasonable" range, and (b) be close to the actual size to be painted on
   * screen.  (The "reasonable" range as determined by some #defines in
   * SVGTextFrame.cpp is 8..200.)
   */
  float mFontSizeScaleFactor;

  /**
   * The scale of the context that we last used to compute mFontSizeScaleFactor.
   * We record this so that we can tell when our scale transform has changed
   * enough to warrant reflowing the text.
   */
  float mLastContextScale;

  /**
   * The amount that we need to scale each rendered run to account for
   * lengthAdjust="spacingAndGlyphs".
   */
  float mLengthAdjustScaleFactor;
};

#endif
