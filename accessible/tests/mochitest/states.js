function testStates(aAccOrElmOrID, aState, aExtraState, aAbsentState)
{
  var [state, extraState] = getStates(aAccOrElmOrID);

  var id = prettyName(aAccOrElmOrID);

  is(state & aState, aState,
     "wrong state bits for " + id + "!");

  if (aExtraState)
    is(extraState & aExtraState, aExtraState,
       "wrong extra state bits for " + id + "!");

  if (aAbsentState)
    is(state & aAbsentState, 0,
       "state bits should not be present in ID " + id + "!");

  if (state & STATE_READONLY)
    is(extraState & EXT_STATE_EDITABLE, 0,
       "Read-only " + id + " cannot be editable!");

  if (extraState & EXT_STATE_EDITABLE)
    is(state & STATE_READONLY, 0,
       "Editable " + id + " cannot be readonly!");

  if (state & STATE_COLLAPSED || state & STATE_EXPANDED)
    is(extraState & EXT_STATE_EXPANDABLE, EXT_STATE_EXPANDABLE,
       "Collapsed or expanded " + id + " should be expandable!");

  if (state & STATE_COLLAPSED)
    is(state & STATE_EXPANDED, 0,
       "Collapsed " + id + " cannot be expanded!");

  if (state & STATE_EXPANDED)
    is(state & STATE_COLLAPSED, 0,
       "Expanded " + id + " cannot be collapsed!");
}

function getStringStates(aAccOrElmOrID)
{
  var [state, extraState] = getStates(aAccOrElmOrID);
  var list = gAccRetrieval.getStringStates(state, extraState);

  var str = "";
  for (var index = 0; index < list.length; index++)
    str += list.item(index) + ", ";

  return str;
}

function getStates(aAccOrElmOrID)
{
  var acc = getAccessible(aAccOrElmOrID);
  if (!acc)
    return [0, 0];
  
  var state = {}, extraState = {};
  acc.getState(state, extraState);

  return [state.value, extraState.value];
}
