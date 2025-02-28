/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=4 sw=2 et tw=78: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsEditorEventListener.h"

#include "mozilla/Assertions.h"         // for MOZ_ASSERT, etc
#include "mozilla/EventListenerManager.h" // for EventListenerManager
#include "mozilla/IMEStateManager.h"    // for IMEStateManager
#include "mozilla/Preferences.h"        // for Preferences
#include "mozilla/TextEvents.h"         // for WidgetCompositionEvent
#include "mozilla/dom/Element.h"        // for Element
#include "mozilla/dom/EventTarget.h"    // for EventTarget
#include "mozilla/dom/Selection.h"
#include "nsAString.h"
#include "nsCaret.h"                    // for nsCaret
#include "nsDebug.h"                    // for NS_ENSURE_TRUE, etc
#include "nsEditor.h"                   // for nsEditor, etc
#include "nsFocusManager.h"             // for nsFocusManager
#include "nsGkAtoms.h"                  // for nsGkAtoms, nsGkAtoms::input
#include "nsIClipboard.h"               // for nsIClipboard, etc
#include "nsIContent.h"                 // for nsIContent
#include "nsIController.h"              // for nsIController
#include "nsID.h"
#include "mozilla/dom/DOMStringList.h"
#include "mozilla/dom/DataTransfer.h"
#include "nsIDOMDocument.h"             // for nsIDOMDocument
#include "nsIDOMDragEvent.h"            // for nsIDOMDragEvent
#include "nsIDOMElement.h"              // for nsIDOMElement
#include "nsIDOMEvent.h"                // for nsIDOMEvent
#include "nsIDOMEventTarget.h"          // for nsIDOMEventTarget
#include "nsIDOMKeyEvent.h"             // for nsIDOMKeyEvent
#include "nsIDOMMouseEvent.h"           // for nsIDOMMouseEvent
#include "nsIDOMNode.h"                 // for nsIDOMNode
#include "nsIDocument.h"                // for nsIDocument
#include "nsIEditor.h"                  // for nsEditor::GetSelection, etc
#include "nsIEditorIMESupport.h"
#include "nsIEditorMailSupport.h"       // for nsIEditorMailSupport
#include "nsIFocusManager.h"            // for nsIFocusManager
#include "nsIFormControl.h"             // for nsIFormControl, etc
#include "nsIHTMLEditor.h"              // for nsIHTMLEditor
#include "nsINode.h"                    // for nsINode, ::NODE_IS_EDITABLE, etc
#include "nsIPlaintextEditor.h"         // for nsIPlaintextEditor, etc
#include "nsIPresShell.h"               // for nsIPresShell
#include "nsISelectionController.h"     // for nsISelectionController, etc
#include "nsITransferable.h"            // for kFileMime, kHTMLMime, etc
#include "nsIWidget.h"                  // for nsIWidget
#include "nsLiteralString.h"            // for NS_LITERAL_STRING
#include "nsPIWindowRoot.h"             // for nsPIWindowRoot
#include "nsPrintfCString.h"            // for nsPrintfCString
#include "nsRange.h"
#include "nsServiceManagerUtils.h"      // for do_GetService
#include "nsString.h"                   // for nsAutoString
#include "nsQueryObject.h"              // for do_QueryObject
#ifdef HANDLE_NATIVE_TEXT_DIRECTION_SWITCH
#include "nsContentUtils.h"             // for nsContentUtils, etc
#include "nsIBidiKeyboard.h"            // for nsIBidiKeyboard
#endif

#include "mozilla/dom/TabParent.h"

class nsPresContext;

using namespace mozilla;
using namespace mozilla::dom;

static void
DoCommandCallback(Command aCommand, void* aData)
{
  nsIDocument* doc = static_cast<nsIDocument*>(aData);
  nsPIDOMWindow* win = doc->GetWindow();
  if (!win) {
    return;
  }
  nsCOMPtr<nsPIWindowRoot> root = win->GetTopWindowRoot();
  if (!root) {
    return;
  }

  const char* commandStr = WidgetKeyboardEvent::GetCommandStr(aCommand);

  nsCOMPtr<nsIController> controller;
  root->GetControllerForCommand(commandStr, getter_AddRefs(controller));
  if (!controller) {
    return;
  }

  bool commandEnabled;
  nsresult rv = controller->IsCommandEnabled(commandStr, &commandEnabled);
  NS_ENSURE_SUCCESS_VOID(rv);
  if (commandEnabled) {
    controller->DoCommand(commandStr);
  }
}

nsEditorEventListener::nsEditorEventListener()
  : mEditor(nullptr)
  , mCommitText(false)
  , mInTransaction(false)
  , mMouseDownOrUpConsumedByIME(false)
#ifdef HANDLE_NATIVE_TEXT_DIRECTION_SWITCH
  , mHaveBidiKeyboards(false)
  , mShouldSwitchTextDirection(false)
  , mSwitchToRTL(false)
#endif
{
}

nsEditorEventListener::~nsEditorEventListener()
{
  if (mEditor) {
    NS_WARNING("We're not uninstalled");
    Disconnect();
  }
}

nsresult
nsEditorEventListener::Connect(nsEditor* aEditor)
{
  NS_ENSURE_ARG(aEditor);

#ifdef HANDLE_NATIVE_TEXT_DIRECTION_SWITCH
  nsIBidiKeyboard* bidiKeyboard = nsContentUtils::GetBidiKeyboard();
  if (bidiKeyboard) {
    bool haveBidiKeyboards = false;
    bidiKeyboard->GetHaveBidiKeyboards(&haveBidiKeyboards);
    mHaveBidiKeyboards = haveBidiKeyboards;
  }
#endif

  mEditor = aEditor;

  nsresult rv = InstallToEditor();
  if (NS_FAILED(rv)) {
    Disconnect();
  }
  return rv;
}

nsresult
nsEditorEventListener::InstallToEditor()
{
  NS_PRECONDITION(mEditor, "The caller must set mEditor");

  nsCOMPtr<EventTarget> piTarget = mEditor->GetDOMEventTarget();
  NS_ENSURE_TRUE(piTarget, NS_ERROR_FAILURE);

  // register the event listeners with the listener manager
  EventListenerManager* elmP = piTarget->GetOrCreateListenerManager();
  NS_ENSURE_STATE(elmP);

#ifdef HANDLE_NATIVE_TEXT_DIRECTION_SWITCH
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("keydown"),
                               TrustedEventsAtSystemGroupBubble());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("keyup"),
                               TrustedEventsAtSystemGroupBubble());
#endif
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("keypress"),
                               TrustedEventsAtSystemGroupBubble());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("dragenter"),
                               TrustedEventsAtSystemGroupBubble());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("dragover"),
                               TrustedEventsAtSystemGroupBubble());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("dragexit"),
                               TrustedEventsAtSystemGroupBubble());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("drop"),
                               TrustedEventsAtSystemGroupBubble());
  // XXX We should add the mouse event listeners as system event group.
  //     E.g., web applications cannot prevent middle mouse paste by
  //     preventDefault() of click event at bubble phase.
  //     However, if we do so, all click handlers in any frames and frontend
  //     code need to check if it's editable.  It makes easier create new bugs.
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("mousedown"),
                               TrustedEventsAtCapture());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("mouseup"),
                               TrustedEventsAtCapture());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("click"),
                               TrustedEventsAtCapture());
// Focus event doesn't bubble so adding the listener to capturing phase.
// Make sure this works after bug 235441 gets fixed.
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("blur"),
                               TrustedEventsAtCapture());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("focus"),
                               TrustedEventsAtCapture());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("text"),
                               TrustedEventsAtSystemGroupBubble());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("compositionstart"),
                               TrustedEventsAtSystemGroupBubble());
  elmP->AddEventListenerByType(this,
                               NS_LITERAL_STRING("compositionend"),
                               TrustedEventsAtSystemGroupBubble());

  return NS_OK;
}

void
nsEditorEventListener::Disconnect()
{
  if (!mEditor) {
    return;
  }
  UninstallFromEditor();

  nsIFocusManager* fm = nsFocusManager::GetFocusManager();
  if (fm) {
    nsCOMPtr<nsIDOMElement> domFocus;
    fm->GetFocusedElement(getter_AddRefs(domFocus));
    nsCOMPtr<nsINode> focusedElement = do_QueryInterface(domFocus);
    mozilla::dom::Element* root = mEditor->GetRoot();
    if (focusedElement && root &&
        nsContentUtils::ContentIsDescendantOf(focusedElement, root)) {
      // Reset the Selection ancestor limiter and SelectionController state
      // that nsEditor::InitializeSelection set up.
      mEditor->FinalizeSelection();
    }
  }

  mEditor = nullptr;
}

void
nsEditorEventListener::UninstallFromEditor()
{
  nsCOMPtr<EventTarget> piTarget = mEditor->GetDOMEventTarget();
  if (!piTarget) {
    return;
  }

  EventListenerManager* elmP = piTarget->GetOrCreateListenerManager();
  if (!elmP) {
    return;
  }

#ifdef HANDLE_NATIVE_TEXT_DIRECTION_SWITCH
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("keydown"),
                                  TrustedEventsAtSystemGroupBubble());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("keyup"),
                                  TrustedEventsAtSystemGroupBubble());
#endif
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("keypress"),
                                  TrustedEventsAtSystemGroupBubble());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("dragenter"),
                                  TrustedEventsAtSystemGroupBubble());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("dragover"),
                                  TrustedEventsAtSystemGroupBubble());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("dragexit"),
                                  TrustedEventsAtSystemGroupBubble());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("drop"),
                                  TrustedEventsAtSystemGroupBubble());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("mousedown"),
                                  TrustedEventsAtCapture());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("mouseup"),
                                  TrustedEventsAtCapture());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("click"),
                                  TrustedEventsAtCapture());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("blur"),
                                  TrustedEventsAtCapture());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("focus"),
                                  TrustedEventsAtCapture());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("text"),
                                  TrustedEventsAtSystemGroupBubble());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("compositionstart"),
                                  TrustedEventsAtSystemGroupBubble());
  elmP->RemoveEventListenerByType(this,
                                  NS_LITERAL_STRING("compositionend"),
                                  TrustedEventsAtSystemGroupBubble());
}

already_AddRefed<nsIPresShell>
nsEditorEventListener::GetPresShell()
{
  NS_PRECONDITION(mEditor,
    "The caller must check whether this is connected to an editor");
  return mEditor->GetPresShell();
}

nsPresContext*
nsEditorEventListener::GetPresContext()
{
  nsCOMPtr<nsIPresShell> presShell = GetPresShell();
  return presShell ? presShell->GetPresContext() : nullptr;
}

nsIContent*
nsEditorEventListener::GetFocusedRootContent()
{
  NS_ENSURE_TRUE(mEditor, nullptr);

  nsCOMPtr<nsIContent> focusedContent = mEditor->GetFocusedContent();
  if (!focusedContent) {
    return nullptr;
  }

  nsIDocument* composedDoc = focusedContent->GetComposedDoc();
  NS_ENSURE_TRUE(composedDoc, nullptr);

  return composedDoc->HasFlag(NODE_IS_EDITABLE) ? nullptr : focusedContent;
}

bool
nsEditorEventListener::EditorHasFocus()
{
  NS_PRECONDITION(mEditor,
    "The caller must check whether this is connected to an editor");
  nsCOMPtr<nsIContent> focusedContent = mEditor->GetFocusedContent();
  if (!focusedContent) {
    return false;
  }
  nsIDocument* composedDoc = focusedContent->GetComposedDoc();
  return !!composedDoc;
}

/**
 *  nsISupports implementation
 */

NS_IMPL_ISUPPORTS(nsEditorEventListener, nsIDOMEventListener)

/**
 *  nsIDOMEventListener implementation
 */

NS_IMETHODIMP
nsEditorEventListener::HandleEvent(nsIDOMEvent* aEvent)
{
  NS_ENSURE_TRUE(mEditor, NS_ERROR_FAILURE);

  nsCOMPtr<nsIEditor> kungFuDeathGrip = mEditor;

  WidgetEvent* internalEvent = aEvent->GetInternalNSEvent();

  // Let's handle each event with the message of the internal event of the
  // coming event.  If the DOM event was created with improper interface,
  // e.g., keydown event is created with |new MouseEvent("keydown", {});|,
  // its message is always 0.  Therefore, we can ban such strange event easy.
  // However, we need to handle strange "focus" and "blur" event.  See the
  // following code of this switch statement.
  // NOTE: Each event handler may require specific event interface.  Before
  //       calling it, this queries the specific interface.  If it would fail,
  //       each event handler would just ignore the event.  So, in this method,
  //       you don't need to check if the QI succeeded before each call.
  switch (internalEvent->mMessage) {
    // dragenter
    case eDragEnter: {
      nsCOMPtr<nsIDOMDragEvent> dragEvent = do_QueryInterface(aEvent);
      return DragEnter(dragEvent);
    }
    // dragover
    case eDragOver: {
      nsCOMPtr<nsIDOMDragEvent> dragEvent = do_QueryInterface(aEvent);
      return DragOver(dragEvent);
    }
    // dragexit
    case eDragExit: {
      nsCOMPtr<nsIDOMDragEvent> dragEvent = do_QueryInterface(aEvent);
      return DragExit(dragEvent);
    }
    // drop
    case eDrop: {
      nsCOMPtr<nsIDOMDragEvent> dragEvent = do_QueryInterface(aEvent);
      return Drop(dragEvent);
    }
#ifdef HANDLE_NATIVE_TEXT_DIRECTION_SWITCH
    // keydown
    case eKeyDown: {
      nsCOMPtr<nsIDOMKeyEvent> keyEvent = do_QueryInterface(aEvent);
      return KeyDown(keyEvent);
    }
    // keyup
    case eKeyUp: {
      nsCOMPtr<nsIDOMKeyEvent> keyEvent = do_QueryInterface(aEvent);
      return KeyUp(keyEvent);
    }
#endif // #ifdef HANDLE_NATIVE_TEXT_DIRECTION_SWITCH
    // keypress
    case eKeyPress: {
      nsCOMPtr<nsIDOMKeyEvent> keyEvent = do_QueryInterface(aEvent);
      return KeyPress(keyEvent);
    }
    // mousedown
    case eMouseDown: {
      nsCOMPtr<nsIDOMMouseEvent> mouseEvent = do_QueryInterface(aEvent);
      NS_ENSURE_TRUE(mouseEvent, NS_OK);
      // nsEditorEventListener may receive (1) all mousedown, mouseup and click
      // events, (2) only mousedown event or (3) only mouseup event.
      // mMouseDownOrUpConsumedByIME is used only for ignoring click event if
      // preceding mousedown and/or mouseup event is consumed by IME.
      // Therefore, even if case #2 or case #3 occurs,
      // mMouseDownOrUpConsumedByIME is true here.  Therefore, we should always
      // overwrite it here.
      mMouseDownOrUpConsumedByIME = NotifyIMEOfMouseButtonEvent(mouseEvent);
      return mMouseDownOrUpConsumedByIME ? NS_OK : MouseDown(mouseEvent);
    }
    // mouseup
    case eMouseUp: {
      nsCOMPtr<nsIDOMMouseEvent> mouseEvent = do_QueryInterface(aEvent);
      NS_ENSURE_TRUE(mouseEvent, NS_OK);
      // See above comment in the eMouseDown case, first.
      // This code assumes that case #1 is occuring.  However, if case #3 may
      // occurs after case #2 and the mousedown is consumed,
      // mMouseDownOrUpConsumedByIME is true even though nsEditorEventListener
      // has not received the preceding mousedown event of this mouseup event.
      // So, mMouseDownOrUpConsumedByIME may be invalid here.  However,
      // this is not a matter because mMouseDownOrUpConsumedByIME is referred
      // only by eMouseClick case but click event is fired only in case #1.
      // So, before a click event is fired, mMouseDownOrUpConsumedByIME is
      // always initialized in the eMouseDown case if it's referred.
      if (NotifyIMEOfMouseButtonEvent(mouseEvent)) {
        mMouseDownOrUpConsumedByIME = true;
      }
      return mMouseDownOrUpConsumedByIME ? NS_OK : MouseUp(mouseEvent);
    }
    // click
    case eMouseClick: {
      nsCOMPtr<nsIDOMMouseEvent> mouseEvent = do_QueryInterface(aEvent);
      NS_ENSURE_TRUE(mouseEvent, NS_OK);
      // If the preceding mousedown event or mouseup event was consumed,
      // editor shouldn't handle this click event.
      if (mMouseDownOrUpConsumedByIME) {
        mMouseDownOrUpConsumedByIME = false;
        mouseEvent->AsEvent()->PreventDefault();
        return NS_OK;
      }
      return MouseClick(mouseEvent);
    }
    // focus
    case eFocus:
      return Focus(aEvent);
    // blur
    case eBlur:
      return Blur(aEvent);
    // text
    case eCompositionChange:
      return HandleText(aEvent);
    // compositionstart
    case eCompositionStart:
      return HandleStartComposition(aEvent);
    // compositionend
    case eCompositionEnd:
      HandleEndComposition(aEvent);
      return NS_OK;
    default:
      break;
  }

  nsAutoString eventType;
  aEvent->GetType(eventType);
  // We should accept "focus" and "blur" event even if it's synthesized with
  // wrong interface for compatibility with older Gecko.
  if (eventType.EqualsLiteral("focus")) {
    return Focus(aEvent);
  }
  if (eventType.EqualsLiteral("blur")) {
    return Blur(aEvent);
  }
#ifdef DEBUG
  nsPrintfCString assertMessage("Editor doesn't handle \"%s\" event "
    "because its internal event doesn't have proper message",
    NS_ConvertUTF16toUTF8(eventType).get());
  NS_ASSERTION(false, assertMessage.get());
#endif

  return NS_OK;
}

#ifdef HANDLE_NATIVE_TEXT_DIRECTION_SWITCH
#include <windows.h>
// Undo the windows.h damage
#undef GetMessage
#undef CreateEvent
#undef GetClassName
#undef GetBinaryType
#undef RemoveDirectory
#undef SetProp

namespace {

// This function is borrowed from Chromium's ImeInput::IsCtrlShiftPressed
bool IsCtrlShiftPressed(bool& isRTL)
{
  BYTE keystate[256];
  if (!::GetKeyboardState(keystate)) {
    return false;
  }

  // To check if a user is pressing only a control key and a right-shift key
  // (or a left-shift key), we use the steps below:
  // 1. Check if a user is pressing a control key and a right-shift key (or
  //    a left-shift key).
  // 2. If the condition 1 is true, we should check if there are any other
  //    keys pressed at the same time.
  //    To ignore the keys checked in 1, we set their status to 0 before
  //    checking the key status.
  const int kKeyDownMask = 0x80;
  if ((keystate[VK_CONTROL] & kKeyDownMask) == 0) {
    return false;
  }

  if (keystate[VK_RSHIFT] & kKeyDownMask) {
    keystate[VK_RSHIFT] = 0;
    isRTL = true;
  } else if (keystate[VK_LSHIFT] & kKeyDownMask) {
    keystate[VK_LSHIFT] = 0;
    isRTL = false;
  } else {
    return false;
  }

  // Scan the key status to find pressed keys. We should abandon changing the
  // text direction when there are other pressed keys.
  // This code is executed only when a user is pressing a control key and a
  // right-shift key (or a left-shift key), i.e. we should ignore the status of
  // the keys: VK_SHIFT, VK_CONTROL, VK_RCONTROL, and VK_LCONTROL.
  // So, we reset their status to 0 and ignore them.
  keystate[VK_SHIFT] = 0;
  keystate[VK_CONTROL] = 0;
  keystate[VK_RCONTROL] = 0;
  keystate[VK_LCONTROL] = 0;
  for (int i = 0; i <= VK_PACKET; ++i) {
    if (keystate[i] & kKeyDownMask) {
      return false;
    }
  }
  return true;
}

}

// This logic is mostly borrowed from Chromium's
// RenderWidgetHostViewWin::OnKeyEvent.

nsresult
nsEditorEventListener::KeyUp(nsIDOMKeyEvent* aKeyEvent)
{
  NS_ENSURE_TRUE(aKeyEvent, NS_OK);

  if (!mHaveBidiKeyboards) {
    return NS_OK;
  }

  uint32_t keyCode = 0;
  aKeyEvent->GetKeyCode(&keyCode);
  if ((keyCode == nsIDOMKeyEvent::DOM_VK_SHIFT ||
       keyCode == nsIDOMKeyEvent::DOM_VK_CONTROL) &&
      mShouldSwitchTextDirection && mEditor->IsPlaintextEditor()) {
    mEditor->SwitchTextDirectionTo(mSwitchToRTL ?
      nsIPlaintextEditor::eEditorRightToLeft :
      nsIPlaintextEditor::eEditorLeftToRight);
    mShouldSwitchTextDirection = false;
  }
  return NS_OK;
}

nsresult
nsEditorEventListener::KeyDown(nsIDOMKeyEvent* aKeyEvent)
{
  NS_ENSURE_TRUE(aKeyEvent, NS_OK);

  if (!mHaveBidiKeyboards) {
    return NS_OK;
  }

  uint32_t keyCode = 0;
  aKeyEvent->GetKeyCode(&keyCode);
  if (keyCode == nsIDOMKeyEvent::DOM_VK_SHIFT) {
    bool switchToRTL;
    if (IsCtrlShiftPressed(switchToRTL)) {
      mShouldSwitchTextDirection = true;
      mSwitchToRTL = switchToRTL;
    }
  } else if (keyCode != nsIDOMKeyEvent::DOM_VK_CONTROL) {
    // In case the user presses any other key besides Ctrl and Shift
    mShouldSwitchTextDirection = false;
  }
  return NS_OK;
}
#endif

nsresult
nsEditorEventListener::KeyPress(nsIDOMKeyEvent* aKeyEvent)
{
  NS_ENSURE_TRUE(aKeyEvent, NS_OK);

  if (!mEditor->IsAcceptableInputEvent(aKeyEvent->AsEvent())) {
    return NS_OK;
  }

  // DOM event handling happens in two passes, the client pass and the system
  // pass.  We do all of our processing in the system pass, to allow client
  // handlers the opportunity to cancel events and prevent typing in the editor.
  // If the client pass cancelled the event, defaultPrevented will be true
  // below.

  bool defaultPrevented;
  aKeyEvent->AsEvent()->GetDefaultPrevented(&defaultPrevented);
  if (defaultPrevented) {
    return NS_OK;
  }

  nsresult rv = mEditor->HandleKeyPressEvent(aKeyEvent);
  NS_ENSURE_SUCCESS(rv, rv);

  aKeyEvent->AsEvent()->GetDefaultPrevented(&defaultPrevented);
  if (defaultPrevented) {
    return NS_OK;
  }

  if (!ShouldHandleNativeKeyBindings(aKeyEvent)) {
    return NS_OK;
  }

  // Now, ask the native key bindings to handle the event.
  WidgetKeyboardEvent* keyEvent =
    aKeyEvent->AsEvent()->GetInternalNSEvent()->AsKeyboardEvent();
  MOZ_ASSERT(keyEvent,
             "DOM key event's internal event must be WidgetKeyboardEvent");
  nsIWidget* widget = keyEvent->widget;
  // If the event is created by chrome script, the widget is always nullptr.
  if (!widget) {
    nsCOMPtr<nsIPresShell> ps = GetPresShell();
    nsPresContext* pc = ps ? ps->GetPresContext() : nullptr;
    widget = pc ? pc->GetNearestWidget() : nullptr;
    NS_ENSURE_TRUE(widget, NS_OK);
  }

  nsCOMPtr<nsIDocument> doc = mEditor->GetDocument();
  bool handled = widget->ExecuteNativeKeyBinding(
                           nsIWidget::NativeKeyBindingsForRichTextEditor,
                           *keyEvent, DoCommandCallback, doc);
  if (handled) {
    aKeyEvent->AsEvent()->PreventDefault();
  }
  return NS_OK;
}

nsresult
nsEditorEventListener::MouseClick(nsIDOMMouseEvent* aMouseEvent)
{
  // nothing to do if editor isn't editable or clicked on out of the editor.
  if (mEditor->IsReadonly() || mEditor->IsDisabled() ||
      !mEditor->IsAcceptableInputEvent(aMouseEvent->AsEvent())) {
    return NS_OK;
  }

  // Notifies clicking on editor to IMEStateManager even when the event was
  // consumed.
  if (EditorHasFocus()) {
    nsPresContext* presContext = GetPresContext();
    if (presContext) {
      IMEStateManager::OnClickInEditor(presContext, GetFocusedRootContent(),
                                       aMouseEvent);
     }
  }

  bool preventDefault;
  nsresult rv = aMouseEvent->AsEvent()->GetDefaultPrevented(&preventDefault);
  if (NS_FAILED(rv) || preventDefault) {
    // We're done if 'preventdefault' is true (see for example bug 70698).
    return rv;
  }

  // If we got a mouse down inside the editing area, we should force the
  // IME to commit before we change the cursor position
  mEditor->ForceCompositionEnd();

  int16_t button = -1;
  aMouseEvent->GetButton(&button);
  if (button == 1) {
    return HandleMiddleClickPaste(aMouseEvent);
  }
  return NS_OK;
}

nsresult
nsEditorEventListener::HandleMiddleClickPaste(nsIDOMMouseEvent* aMouseEvent)
{
  if (!Preferences::GetBool("middlemouse.paste", false)) {
    // Middle click paste isn't enabled.
    return NS_OK;
  }

  // Set the selection to the point under the mouse cursor:
  nsCOMPtr<nsIDOMNode> parent;
  if (NS_FAILED(aMouseEvent->GetRangeParent(getter_AddRefs(parent)))) {
    return NS_ERROR_NULL_POINTER;
  }
  int32_t offset = 0;
  if (NS_FAILED(aMouseEvent->GetRangeOffset(&offset))) {
    return NS_ERROR_NULL_POINTER;
  }

  RefPtr<Selection> selection = mEditor->GetSelection();
  if (selection) {
    selection->Collapse(parent, offset);
  }

  // If the ctrl key is pressed, we'll do paste as quotation.
  // Would've used the alt key, but the kde wmgr treats alt-middle specially.
  bool ctrlKey = false;
  aMouseEvent->GetCtrlKey(&ctrlKey);

  nsCOMPtr<nsIEditorMailSupport> mailEditor;
  if (ctrlKey) {
    mailEditor = do_QueryObject(mEditor);
  }

  nsresult rv;
  int32_t clipboard = nsIClipboard::kGlobalClipboard;
  nsCOMPtr<nsIClipboard> clipboardService =
    do_GetService("@mozilla.org/widget/clipboard;1", &rv);
  if (NS_SUCCEEDED(rv)) {
    bool selectionSupported;
    rv = clipboardService->SupportsSelectionClipboard(&selectionSupported);
    if (NS_SUCCEEDED(rv) && selectionSupported) {
      clipboard = nsIClipboard::kSelectionClipboard;
    }
  }

  if (mailEditor) {
    mailEditor->PasteAsQuotation(clipboard);
  } else {
    mEditor->Paste(clipboard);
  }

  // Prevent the event from propagating up to be possibly handled
  // again by the containing window:
  aMouseEvent->AsEvent()->StopPropagation();
  aMouseEvent->AsEvent()->PreventDefault();

  // We processed the event, whether drop/paste succeeded or not
  return NS_OK;
}

bool
nsEditorEventListener::NotifyIMEOfMouseButtonEvent(
                         nsIDOMMouseEvent* aMouseEvent)
{
  if (!EditorHasFocus()) {
    return false;
  }

  bool defaultPrevented;
  nsresult rv = aMouseEvent->AsEvent()->GetDefaultPrevented(&defaultPrevented);
  NS_ENSURE_SUCCESS(rv, false);
  if (defaultPrevented) {
    return false;
  }
  nsPresContext* presContext = GetPresContext();
  NS_ENSURE_TRUE(presContext, false);
  return IMEStateManager::OnMouseButtonEventInEditor(presContext,
                                                     GetFocusedRootContent(),
                                                     aMouseEvent);
}

nsresult
nsEditorEventListener::MouseDown(nsIDOMMouseEvent* aMouseEvent)
{
  // FYI: This may be called by nsHTMLEditorEventListener::MouseDown() even
  //      when the event is not acceptable for committing composition.
  mEditor->ForceCompositionEnd();
  return NS_OK;
}

nsresult
nsEditorEventListener::HandleText(nsIDOMEvent* aTextEvent)
{
  if (!mEditor->IsAcceptableInputEvent(aTextEvent)) {
    return NS_OK;
  }

  // if we are readonly or disabled, then do nothing.
  if (mEditor->IsReadonly() || mEditor->IsDisabled()) {
    return NS_OK;
  }

  return mEditor->UpdateIMEComposition(aTextEvent);
}

/**
 * Drag event implementation
 */

nsresult
nsEditorEventListener::DragEnter(nsIDOMDragEvent* aDragEvent)
{
  NS_ENSURE_TRUE(aDragEvent, NS_OK);

  nsCOMPtr<nsIPresShell> presShell = GetPresShell();
  NS_ENSURE_TRUE(presShell, NS_OK);

  if (!mCaret) {
    mCaret = new nsCaret();
    mCaret->Init(presShell);
    mCaret->SetCaretReadOnly(true);
    // This is to avoid the requirement that the Selection is Collapsed which
    // it can't be when dragging a selection in the same shell.
    // See nsCaret::IsVisible().
    mCaret->SetVisibilityDuringSelection(true);
  }

  presShell->SetCaret(mCaret);

  return DragOver(aDragEvent);
}

nsresult
nsEditorEventListener::DragOver(nsIDOMDragEvent* aDragEvent)
{
  NS_ENSURE_TRUE(aDragEvent, NS_OK);

  nsCOMPtr<nsIDOMNode> parent;
  bool defaultPrevented;
  aDragEvent->AsEvent()->GetDefaultPrevented(&defaultPrevented);
  if (defaultPrevented) {
    return NS_OK;
  }

  aDragEvent->GetRangeParent(getter_AddRefs(parent));
  nsCOMPtr<nsIContent> dropParent = do_QueryInterface(parent);
  NS_ENSURE_TRUE(dropParent, NS_ERROR_FAILURE);

  if (dropParent->IsEditable() && CanDrop(aDragEvent)) {
    aDragEvent->AsEvent()->PreventDefault(); // consumed

    if (!mCaret) {
      return NS_OK;
    }

    int32_t offset = 0;
    nsresult rv = aDragEvent->GetRangeOffset(&offset);
    NS_ENSURE_SUCCESS(rv, rv);

    mCaret->SetVisible(true);
    mCaret->SetCaretPosition(parent, offset);

    return NS_OK;
  }

  if (!IsFileControlTextBox()) {
    // This is needed when dropping on an input, to prevent the editor for
    // the editable parent from receiving the event.
    aDragEvent->AsEvent()->StopPropagation();
  }

  if (mCaret) {
    mCaret->SetVisible(false);
  }
  return NS_OK;
}

void
nsEditorEventListener::CleanupDragDropCaret()
{
  if (!mCaret) {
    return;
  }

  mCaret->SetVisible(false);    // hide it, so that it turns off its timer

  nsCOMPtr<nsIPresShell> presShell = GetPresShell();
  if (presShell) {
    presShell->RestoreCaret();
  }

  mCaret->Terminate();
  mCaret = nullptr;
}

nsresult
nsEditorEventListener::DragExit(nsIDOMDragEvent* aDragEvent)
{
  NS_ENSURE_TRUE(aDragEvent, NS_OK);

  CleanupDragDropCaret();

  return NS_OK;
}

nsresult
nsEditorEventListener::Drop(nsIDOMDragEvent* aDragEvent)
{
  NS_ENSURE_TRUE(aDragEvent, NS_OK);

  CleanupDragDropCaret();

  bool defaultPrevented;
  aDragEvent->AsEvent()->GetDefaultPrevented(&defaultPrevented);
  if (defaultPrevented) {
    return NS_OK;
  }

  nsCOMPtr<nsIDOMNode> parent;
  aDragEvent->GetRangeParent(getter_AddRefs(parent));
  nsCOMPtr<nsIContent> dropParent = do_QueryInterface(parent);
  NS_ENSURE_TRUE(dropParent, NS_ERROR_FAILURE);

  if (!dropParent->IsEditable() || !CanDrop(aDragEvent)) {
    // was it because we're read-only?
    if ((mEditor->IsReadonly() || mEditor->IsDisabled()) &&
        !IsFileControlTextBox()) {
      // it was decided to "eat" the event as this is the "least surprise"
      // since someone else handling it might be unintentional and the
      // user could probably re-drag to be not over the disabled/readonly
      // editfields if that is what is desired.
      return aDragEvent->AsEvent()->StopPropagation();
    }
    return NS_OK;
  }

  aDragEvent->AsEvent()->StopPropagation();
  aDragEvent->AsEvent()->PreventDefault();
  return mEditor->InsertFromDrop(aDragEvent->AsEvent());
}

bool
nsEditorEventListener::CanDrop(nsIDOMDragEvent* aEvent)
{
  // if the target doc is read-only, we can't drop
  if (mEditor->IsReadonly() || mEditor->IsDisabled()) {
    return false;
  }

  nsCOMPtr<nsIDOMDataTransfer> domDataTransfer;
  aEvent->GetDataTransfer(getter_AddRefs(domDataTransfer));
  nsCOMPtr<DataTransfer> dataTransfer = do_QueryInterface(domDataTransfer);
  NS_ENSURE_TRUE(dataTransfer, false);

  RefPtr<DOMStringList> types = dataTransfer->Types();

  // Plaintext editors only support dropping text. Otherwise, HTML and files
  // can be dropped as well.
  if (!types->Contains(NS_LITERAL_STRING(kTextMime)) &&
      !types->Contains(NS_LITERAL_STRING(kMozTextInternal)) &&
      (mEditor->IsPlaintextEditor() ||
       (!types->Contains(NS_LITERAL_STRING(kHTMLMime)) &&
        !types->Contains(NS_LITERAL_STRING(kFileMime))))) {
    return false;
  }

  // If there is no source node, this is probably an external drag and the
  // drop is allowed. The later checks rely on checking if the drag target
  // is the same as the drag source.
  nsCOMPtr<nsIDOMNode> sourceNode;
  dataTransfer->GetMozSourceNode(getter_AddRefs(sourceNode));
  if (!sourceNode) {
    return true;
  }

  // There is a source node, so compare the source documents and this document.
  // Disallow drops on the same document.

  nsCOMPtr<nsIDOMDocument> domdoc = mEditor->GetDOMDocument();
  NS_ENSURE_TRUE(domdoc, false);

  nsCOMPtr<nsIDOMDocument> sourceDoc;
  nsresult rv = sourceNode->GetOwnerDocument(getter_AddRefs(sourceDoc));
  NS_ENSURE_SUCCESS(rv, false);

  // If the source and the dest are not same document, allow to drop it always.
  if (domdoc != sourceDoc) {
    return true;
  }

  // If the source node is a remote browser, treat this as coming from a
  // different document and allow the drop.
  nsCOMPtr<nsIContent> sourceContent = do_QueryInterface(sourceNode);
  TabParent* tp = TabParent::GetFrom(sourceContent);
  if (tp) {
    return true;
  }

  RefPtr<Selection> selection = mEditor->GetSelection();
  if (!selection) {
    return false;
  }

  // If selection is collapsed, allow to drop it always.
  if (selection->Collapsed()) {
    return true;
  }

  nsCOMPtr<nsIDOMNode> parent;
  rv = aEvent->GetRangeParent(getter_AddRefs(parent));
  if (NS_FAILED(rv) || !parent) {
    return false;
  }

  int32_t offset = 0;
  rv = aEvent->GetRangeOffset(&offset);
  NS_ENSURE_SUCCESS(rv, false);

  int32_t rangeCount;
  rv = selection->GetRangeCount(&rangeCount);
  NS_ENSURE_SUCCESS(rv, false);

  for (int32_t i = 0; i < rangeCount; i++) {
    RefPtr<nsRange> range = selection->GetRangeAt(i);
    if (!range) {
      // Don't bail yet, iterate through them all
      continue;
    }

    bool inRange = true;
    range->IsPointInRange(parent, offset, &inRange);
    if (inRange) {
      // Okay, now you can bail, we are over the orginal selection
      return false;
    }
  }
  return true;
}

nsresult
nsEditorEventListener::HandleStartComposition(nsIDOMEvent* aCompositionEvent)
{
  if (!mEditor->IsAcceptableInputEvent(aCompositionEvent)) {
    return NS_OK;
  }
  WidgetCompositionEvent* compositionStart =
    aCompositionEvent->GetInternalNSEvent()->AsCompositionEvent();
  return mEditor->BeginIMEComposition(compositionStart);
}

void
nsEditorEventListener::HandleEndComposition(nsIDOMEvent* aCompositionEvent)
{
  if (!mEditor->IsAcceptableInputEvent(aCompositionEvent)) {
    return;
  }

  mEditor->EndIMEComposition();
}

nsresult
nsEditorEventListener::Focus(nsIDOMEvent* aEvent)
{
  NS_ENSURE_TRUE(aEvent, NS_OK);

  // Don't turn on selection and caret when the editor is disabled.
  if (mEditor->IsDisabled()) {
    return NS_OK;
  }

  // Spell check a textarea the first time that it is focused.
  SpellCheckIfNeeded();
  if (!mEditor) {
    // In e10s, this can cause us to flush notifications, which can destroy
    // the node we're about to focus.
    return NS_OK;
  }

  nsCOMPtr<nsIDOMEventTarget> target;
  aEvent->GetTarget(getter_AddRefs(target));
  nsCOMPtr<nsINode> node = do_QueryInterface(target);
  NS_ENSURE_TRUE(node, NS_ERROR_UNEXPECTED);

  // If the target is a document node but it's not editable, we should ignore
  // it because actual focused element's event is going to come.
  if (node->IsNodeOfType(nsINode::eDOCUMENT) &&
      !node->HasFlag(NODE_IS_EDITABLE)) {
    return NS_OK;
  }

  if (node->IsNodeOfType(nsINode::eCONTENT)) {
    // XXX If the focus event target is a form control in contenteditable
    // element, perhaps, the parent HTML editor should do nothing by this
    // handler.  However, FindSelectionRoot() returns the root element of the
    // contenteditable editor.  So, the editableRoot value is invalid for
    // the plain text editor, and it will be set to the wrong limiter of
    // the selection.  However, fortunately, actual bugs are not found yet.
    nsCOMPtr<nsIContent> editableRoot = mEditor->FindSelectionRoot(node);

    // make sure that the element is really focused in case an earlier
    // listener in the chain changed the focus.
    if (editableRoot) {
      nsIFocusManager* fm = nsFocusManager::GetFocusManager();
      NS_ENSURE_TRUE(fm, NS_OK);

      nsCOMPtr<nsIDOMElement> element;
      fm->GetFocusedElement(getter_AddRefs(element));
      if (!SameCOMIdentity(element, target)) {
        return NS_OK;
      }
    }
  }

  mEditor->OnFocus(target);

  nsCOMPtr<nsIPresShell> ps = GetPresShell();
  NS_ENSURE_TRUE(ps, NS_OK);
  nsCOMPtr<nsIContent> focusedContent = mEditor->GetFocusedContentForIME();
  IMEStateManager::OnFocusInEditor(ps->GetPresContext(), focusedContent,
                                   mEditor);

  return NS_OK;
}

nsresult
nsEditorEventListener::Blur(nsIDOMEvent* aEvent)
{
  NS_ENSURE_TRUE(aEvent, NS_OK);

  // check if something else is focused. If another element is focused, then
  // we should not change the selection.
  nsIFocusManager* fm = nsFocusManager::GetFocusManager();
  NS_ENSURE_TRUE(fm, NS_OK);

  nsCOMPtr<nsIDOMElement> element;
  fm->GetFocusedElement(getter_AddRefs(element));
  if (!element) {
    mEditor->FinalizeSelection();
  }
  return NS_OK;
}

void
nsEditorEventListener::SpellCheckIfNeeded()
{
  // If the spell check skip flag is still enabled from creation time,
  // disable it because focused editors are allowed to spell check.
  uint32_t currentFlags = 0;
  mEditor->GetFlags(&currentFlags);
  if(currentFlags & nsIPlaintextEditor::eEditorSkipSpellCheck) {
    currentFlags ^= nsIPlaintextEditor::eEditorSkipSpellCheck;
    mEditor->SetFlags(currentFlags);
  }
}

bool
nsEditorEventListener::IsFileControlTextBox()
{
  dom::Element* root = mEditor->GetRoot();
  if (!root || !root->ChromeOnlyAccess()) {
    return false;
  }
  nsIContent* parent = root->FindFirstNonChromeOnlyAccessContent();
  if (!parent || !parent->IsHTMLElement(nsGkAtoms::input)) {
    return false;
  }
  nsCOMPtr<nsIFormControl> formControl = do_QueryInterface(parent);
  return formControl->GetType() == NS_FORM_INPUT_FILE;
}

bool
nsEditorEventListener::ShouldHandleNativeKeyBindings(nsIDOMKeyEvent* aKeyEvent)
{
  // Only return true if the target of the event is a desendant of the active
  // editing host in order to match the similar decision made in
  // nsXBLWindowKeyHandler.
  // Note that IsAcceptableInputEvent doesn't check for the active editing
  // host for keyboard events, otherwise this check would have been
  // unnecessary.  IsAcceptableInputEvent currently makes a similar check for
  // mouse events.

  nsCOMPtr<nsIDOMEventTarget> target;
  aKeyEvent->AsEvent()->GetTarget(getter_AddRefs(target));
  nsCOMPtr<nsIContent> targetContent = do_QueryInterface(target);
  if (!targetContent) {
    return false;
  }

  nsCOMPtr<nsIHTMLEditor> htmlEditor =
    do_QueryInterface(static_cast<nsIEditor*>(mEditor));
  if (!htmlEditor) {
    return false;
  }

  nsCOMPtr<nsIDocument> doc = mEditor->GetDocument();
  if (doc->HasFlag(NODE_IS_EDITABLE)) {
    // Don't need to perform any checks in designMode documents.
    return true;
  }

  nsIContent* editingHost = htmlEditor->GetActiveEditingHost();
  if (!editingHost) {
    return false;
  }

  return nsContentUtils::ContentIsDescendantOf(targetContent, editingHost);
}
