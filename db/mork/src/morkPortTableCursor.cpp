/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- 
 * 
 * The contents of this file are subject to the Netscape Public License 
 * Version 1.0 (the "NPL"); you may not use this file except in 
 * compliance with the NPL.  You may obtain a copy of the NPL at 
 * http://www.mozilla.org/NPL/ 
 * 
 * Software distributed under the NPL is distributed on an "AS IS" basis, 
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL 
 * for the specific language governing rights and limitations under the 
 * NPL. 
 * 
 * The Initial Developer of this code under the NPL is Netscape 
 * Communications Corporation.  Portions created by Netscape are 
 * Copyright (C) 1999 Netscape Communications Corporation.  All Rights 
 * Reserved. 
 */

#ifndef _MDB_
#include "mdb.h"
#endif

#ifndef _MORK_
#include "mork.h"
#endif

#ifndef _MORKNODE_
#include "morkNode.h"
#endif

#ifndef _MORKENV_
#include "morkEnv.h"
#endif

#ifndef _MORKCURSOR_
#include "morkCursor.h"
#endif

#ifndef _MORKPORTTABLECURSOR_
#include "morkPortTableCursor.h"
#endif

#ifndef _ORKINPORTTABLECURSOR_
#include "orkinPortTableCursor.h"
#endif

#ifndef _MORKSTORE_
#include "morkStore.h"
#endif

//3456789_123456789_123456789_123456789_123456789_123456789_123456789_123456789

// ````` ````` ````` ````` ````` 
// { ===== begin morkNode interface =====

/*public virtual*/ void
morkPortTableCursor::CloseMorkNode(morkEnv* ev) // ClosePortTableCursor() only if open
{
  if ( this->IsOpenNode() )
  {
    this->MarkClosing();
    this->ClosePortTableCursor(ev);
    this->MarkShut();
  }
}

/*public virtual*/
morkPortTableCursor::~morkPortTableCursor() // ClosePortTableCursor() executed earlier
{
  MORK_ASSERT(this->IsShutNode());
}

/*public non-poly*/
morkPortTableCursor::morkPortTableCursor(morkEnv* ev,
  const morkUsage& inUsage,
  nsIMdbHeap* ioHeap, morkStore* ioStore, mdb_scope inRowScope,
  mdb_kind inTableKind, nsIMdbHeap* ioSlotHeap)
: morkCursor(ev, inUsage, ioHeap)
, mPortTableCursor_Store( 0 )
, mPortTableCursor_RowScope( inRowScope )
, mPortTableCursor_TableKind( inTableKind )
{
  if ( ev->Good() )
  {
    if ( ioStore )
    {
      mCursor_Pos = -1;
      mCursor_Seed = 0; // let the iterator do it's own seed handling
      morkStore::SlotWeakStore(ioStore, ev, &mPortTableCursor_Store);
      if ( ev->Good() )
        mNode_Derived = morkDerived_kPortTableCursor;
    }
    else
      ev->NilPointerError();
  }
}

/*public non-poly*/ void
morkPortTableCursor::ClosePortTableCursor(morkEnv* ev) 
{
  if ( this )
  {
    if ( this->IsNode() )
    {
      mCursor_Pos = -1;
      mCursor_Seed = 0;
      morkStore::SlotWeakStore((morkStore*) 0, ev, &mPortTableCursor_Store);
      this->CloseCursor(ev);
      this->MarkShut();
    }
    else
      this->NonNodeError(ev);
  }
  else
    ev->NilPointerError();
}

// } ===== end morkNode methods =====
// ````` ````` ````` ````` ````` 

/*static*/ void
morkPortTableCursor::NonPortTableCursorTypeError(morkEnv* ev)
{
  ev->NewError("non morkPortTableCursor");
}

orkinPortTableCursor*
morkPortTableCursor::AcquirePortTableCursorHandle(morkEnv* ev)
{
  orkinPortTableCursor* outCursor = 0;
  orkinPortTableCursor* c = (orkinPortTableCursor*) mObject_Handle;
  if ( c ) // have an old handle?
    c->AddStrongRef(ev->AsMdbEnv());
  else // need new handle?
  {
    c = orkinPortTableCursor::MakePortTableCursor(ev, this);
    mObject_Handle = c;
  }
  if ( c )
    outCursor = c;
  return outCursor;
}


//3456789_123456789_123456789_123456789_123456789_123456789_123456789_123456789
