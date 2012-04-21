// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
//
// DESCRIPTION:
//      Movement, collision handling.
//      Shooting and aiming.
//
//-----------------------------------------------------------------------------
#ifdef RCSID
static const char
rcsid[] = "$Id$";
#endif

#include <stdlib.h>

#include "m_fixed.h"
#include "m_random.h"
#include "i_system.h"
#include "m_misc.h"
#include "doomdef.h"
#include "p_local.h"
#include "s_sound.h"
#include "doomstat.h"
#include "r_local.h"
#include "sounds.h"
#include "tables.h"
#include "r_sky.h"
#include "con_console.h"
#include "m_math.h"


fixed_t         tmbbox[4];
mobj_t*         tmthing;
int             tmflags;
fixed_t         tmx;
fixed_t         tmy;

mobj_t*			blockthing;


// If "floatok" true, move would be ok
// if within "tmfloorz - tmceilingz".
dboolean                floatok;

fixed_t         tmfloorz;
fixed_t         tmceilingz;
fixed_t         tmdropoffz;

// keep track of the line that lowers the ceiling,
// so missiles don't explode against sky hack walls
line_t*         tmhitline;

// keep track of special lines as they are hit,
// but don't process them until the move is proven valid

line_t*         spechit[MAXSPECIALCROSS];
int             numspechit=0;

//
// P_CheckThingCollision
//

d_inline
static dboolean P_CheckThingCollision(mobj_t *thing)
{
    fixed_t blockdist;
    
    if(netgame && (tmthing->type == MT_PLAYER && thing->type == MT_PLAYER))
        blockdist = (thing->radius>>1) + (tmthing->radius>>1);
    else
        blockdist = thing->radius + tmthing->radius;
    
    if(D_abs(thing->x - tmx) >= blockdist ||
        D_abs(thing->y - tmy) >= blockdist)
    {
        // didn't hit it
        return true;
    }
    
    return false;
}

//
// P_BlockMapBox
//

d_inline
static void P_BlockMapBox(fixed_t* bbox, fixed_t x, fixed_t y, mobj_t* thing)
{
    fixed_t extent = compat_collision.value > 0 ? 0 : MAXRADIUS;

    tmbbox[BOXTOP]      = y + thing->radius;
    tmbbox[BOXBOTTOM]   = y - thing->radius;
    tmbbox[BOXRIGHT]    = x + thing->radius;
    tmbbox[BOXLEFT]     = x - thing->radius;

    bbox[BOXLEFT]       = (tmbbox[BOXLEFT] - bmaporgx - extent) >> MAPBLOCKSHIFT;
    bbox[BOXRIGHT]      = (tmbbox[BOXRIGHT] - bmaporgx + extent) >> MAPBLOCKSHIFT;
    bbox[BOXBOTTOM]     = (tmbbox[BOXBOTTOM] - bmaporgy - extent) >> MAPBLOCKSHIFT;
    bbox[BOXTOP]        = (tmbbox[BOXTOP] - bmaporgy + extent) >> MAPBLOCKSHIFT;

    if(bbox[BOXLEFT] < 0)
        bbox[BOXLEFT] = 0;

    if(bbox[BOXRIGHT] < 0)
        bbox[BOXRIGHT] = 0;

    if(bbox[BOXLEFT] >= bmapwidth)
        bbox[BOXLEFT] = (bmapwidth - 1);

    if(bbox[BOXRIGHT] >= bmapwidth)
        bbox[BOXRIGHT] = (bmapwidth - 1);

    if(bbox[BOXBOTTOM] < 0)
        bbox[BOXBOTTOM] = 0;

    if(bbox[BOXTOP] < 0)
        bbox[BOXTOP] = 0;

    if(bbox[BOXBOTTOM] >= bmapheight)
        bbox[BOXBOTTOM] = (bmapheight - 1);

    if(bbox[BOXTOP] >= bmapheight)
        bbox[BOXTOP] = (bmapheight - 1);
}



//
// MOVEMENT ITERATOR FUNCTIONS
//


//
// PIT_CheckLine
// Adjusts tmfloorz and tmceilingz as lines are contacted
//

dboolean PIT_CheckLine(line_t* ld)
{
    sector_t* sector;

    if(tmbbox[BOXRIGHT] <= ld->bbox[BOXLEFT]
        || tmbbox[BOXLEFT] >= ld->bbox[BOXRIGHT]
        || tmbbox[BOXTOP] <= ld->bbox[BOXBOTTOM]
        || tmbbox[BOXBOTTOM] >= ld->bbox[BOXTOP] )
        return true;
    
    if(P_BoxOnLineSide(tmbbox, ld) != -1)
        return true;
    
    // A line has been hit
    
    // The moving thing's destination position will cross
    // the given line.
    // If this should not be allowed, return false.
    // If the line is special, keep track of it
    // to process later if the move is proven ok.
    // NOTE: specials are NOT sorted by order,
    // so two special lines that are only 8 pixels apart
    // could be crossed in either order.
    
    if(!ld->backsector)
    {
        if(tmthing->flags & MF_MISSILE)
            tmhitline = ld;

        return false;           // one sided line
    }
    
    if(!(tmthing->flags & MF_MISSILE))
    {
        if(ld->flags & ML_BLOCKING)
            return false;       // explicitly blocking everything
        
        if(!tmthing->player && ld->flags & ML_BLOCKMONSTERS)
            return false;       // block monsters only
    }

    // [d64] don't cross mid-pegged lines
    if(ld->flags & ML_DONTPEGMID)
    {
        tmhitline = ld;
        return false;
    }

    // [kex] check if thing's midpoint is inside sector
    if(tmthing->blockflag & BF_MIDPOINTONLY && ld->backsector)
    {
        if(tmthing->subsector->sector != ld->backsector)
            return true;
    }

    sector = ld->frontsector;

    // [d64] check for valid sector heights
    if(sector->ceilingheight == sector->floorheight)
        return false;

    sector = ld->backsector;

    // [d64] check for valid sector heights
    if(sector->ceilingheight == sector->floorheight)
        return false;
    
    // set openrange, opentop, openbottom

    if(((ld->frontsector->floorplane.a | ld->frontsector->floorplane.b) |
        (ld->backsector->floorplane.a | ld->backsector->floorplane.b) |
        (ld->frontsector->ceilingplane.a | ld->frontsector->ceilingplane.b) |
        (ld->backsector->ceilingplane.a | ld->backsector->ceilingplane.b)) == 0)
    {
        P_LineOpening(ld, tmx, tmy, tmx, tmy);
    }
    else
    {
        // Find the point on the line closest to the actor's center, and use
        // that to calculate openings

        float dx = (float)ld->dx;
        float dy = (float)ld->dy;
        float tx = (float)(tmx - ld->v1->x);
        float ty = (float)(tmy - ld->v1->y);
        fixed_t r = (fixed_t)((tx * dx + ty * dy) / (dx * dx + dy * dy) * (float)INT2F(256));

        if(r <= 0)
        {
            P_LineOpening(ld, ld->v1->x, ld->v1->y, tmx, tmy);
        }
        else if(r >= (1<<24))
        {
            P_LineOpening(ld, ld->v2->x, ld->v2->y, tmthing->x, tmthing->y);
        }
        else
        {
            P_LineOpening(ld, ld->v1->x + (FixedMul(r, ld->dx) >> 8),
                ld->v1->y + (FixedMul(r, ld->dy) >> 8), tmx, tmy);
        }
    }
    
    // adjust floor / ceiling heights
    if(opentop < tmceilingz)
    {
        tmceilingz = opentop;
        tmhitline = ld;
    }
    
    if(openbottom > tmfloorz)
        tmfloorz = openbottom;
    
    if(lowfloor < tmdropoffz)
        tmdropoffz = lowfloor;
    
    // if contacted a special line, add it to the list
    if(ld->special & MLU_CROSS)
    {
        if(numspechit > MAXSPECIALCROSS)
        {
            CON_Warnf("PIT_CheckLine: spechit overflow!\n");
        }
        else
        {
            spechit[numspechit] = ld;
            numspechit++;
        }
    }
    
    return true;
}

//
// PIT_CheckThing
//

dboolean PIT_CheckThing(mobj_t* thing)
{
    dboolean    solid;
    
    if(!(thing->flags & (MF_SOLID|MF_SPECIAL|MF_SHOOTABLE)))
        return true;
    
    if(P_CheckThingCollision(thing))
        return true;
    
    // don't clip against self
    if(thing == tmthing)
        return true;
    
    if(tmthing->blockflag & BF_MOBJPASS)
    {
        if(tmthing->z >= thing->z + thing->height && !(thing->flags & MF_SPECIAL))
            return true;
        else if(tmthing->z + tmthing->height < thing->z && !(thing->flags & MF_SPECIAL))
            return true;
    }

    // skull slammed into somthing...
    if(tmthing->flags & MF_SKULLFLY)
    {
        blockthing = thing;
        return false;
    }
    
    // missiles can hit other things
    if(tmthing->flags & MF_MISSILE)
    {
        // see if it went over / under
        if(tmthing->z > thing->z + thing->height)
            return true;                // overhead
        if(tmthing->z + tmthing->height < thing->z)
            return true;                // underneath
        
        if(tmthing->target && tmthing->target->type == thing->type)
        {
            // Don't hit same species as originator.
            if(thing == tmthing->target)
                return true;
            
            // Explode, but do no damage.
            // Let players missile other players.
            if(thing->type != MT_PLAYER)
                return false;
        }
        
        if(!(thing->flags & MF_SHOOTABLE))
        {
            // didn't do any damage
            return !(thing->flags & MF_SOLID);
        }

        blockthing = thing;
        
        // don't traverse any more
        return false;
    }
    
    // check for special pickup
    if(thing->flags & MF_SPECIAL)
    {
        solid = thing->flags & MF_SOLID;
        if(tmflags & MF_PICKUP)
        {
            if(thing->z <= (tmthing->z + (tmthing->height>>1)) ||
                thing->flags & MF_NOSECTOR)
            {
                // can remove thing
                P_TouchSpecialThing(thing, tmthing);
            }
        }
        return !solid;
    }
    
    return !(thing->flags & MF_SOLID);
}


//
// MOVEMENT CLIPPING
//

//
// P_CheckPosition
// This is purely informative, nothing is modified
// (except things picked up).
//
// in:
//  a mobj_t (can be valid or invalid)
//  a position to be checked
//   (doesn't need to be related to the mobj_t->x,y)
//
// during:
//  special things are touched if MF_PICKUP
//  early out on solid lines?
//
// out:
//  newsubsec
//  floorz
//  ceilingz
//  tmdropoffz
//   the lowest point contacted
//   (monsters won't move to a dropoff)
//  speciallines[]
//  numspeciallines
//
dboolean P_CheckPosition(mobj_t* thing, fixed_t x, fixed_t y)
{
    int             bx;
    int             by;
    subsector_t*    newsubsec;
    fixed_t         bbox[4];
    
    tmthing = thing;
    tmflags = thing->flags;
    
    tmx = x;
    tmy = y;
    
    newsubsec = R_PointInSubsector(x, y);

    tmhitline = NULL;
    blockthing = NULL;
    
    // The base floor / ceiling is from the subsector
    // that contains the point.
    // Any contacted lines the step closer together
    // will adjust them.
    tmfloorz = tmdropoffz = M_PointToZ(&newsubsec->sector->floorplane, x, y);
    tmceilingz = M_PointToZ(&newsubsec->sector->ceilingplane, x, y);
    
    D_IncValidCount();
    numspechit = 0;
    
    if (tmflags & MF_NOCLIP)
        return true;
    
    // Check things first, possibly picking things up.
    // [d64] MAXRADIUS is not used
	P_BlockMapBox(bbox, x, y, tmthing);
    
    for(bx = bbox[BOXLEFT]; bx <= bbox[BOXRIGHT]; bx++)
    {
        for(by = bbox[BOXBOTTOM]; by <= bbox[BOXTOP]; by++)
        {
            if(!P_BlockThingsIterator(bx, by, PIT_CheckThing))
                return false;
        }
    }

    // check lines
    for(bx = bbox[BOXLEFT]; bx <= bbox[BOXRIGHT]; bx++)
    {
        for(by = bbox[BOXBOTTOM]; by <= bbox[BOXTOP]; by++)
        {
            if(!P_BlockLinesIterator(bx, by, PIT_CheckLine))
                return false;
        }
    }
                    
    return true;
}


//
// P_TryMove
// Attempt to move to a new position,
// crossing special lines unless MF_TELEPORT is set.
//

dboolean P_TryMove(mobj_t* thing, fixed_t x, fixed_t y)
{
    fixed_t     oldx;
    fixed_t     oldy;
    int         side;
    int         oldside;
    line_t*     ld;
    
    floatok = false;

    if(!P_CheckPosition(thing, x, y))
        return false;           // solid wall or thing
    
    if(!(thing->flags & MF_NOCLIP))
    {
        if(tmceilingz - tmfloorz < thing->height)
            return false;       // doesn't fit
        
        floatok = true;
        
        if(!(thing->flags&MF_TELEPORT)
            && tmceilingz - thing->z < thing->height)
            return false;       // mobj must lower itself to fit
        
        if(!(thing->flags&MF_TELEPORT)
            && tmfloorz - thing->z > 24*FRACUNIT )
            return false;       // too big a step up
        
        if(!(thing->flags&(MF_DROPOFF|MF_FLOAT))
            && tmfloorz - tmdropoffz > 24*FRACUNIT)
            return false;       // don't stand over a dropoff
    }
    
    // the move is ok,
    // so link the thing into its new position
    P_UnsetThingPosition(thing);
    
    oldx = thing->x;
    oldy = thing->y;
    thing->floorz = tmfloorz;
    thing->ceilingz = tmceilingz;
    thing->x = x;
    thing->y = y;
    
    P_SetThingPosition(thing);
    
    // if any special lines were hit, do the effect
    if(!(thing->flags&(MF_TELEPORT|MF_NOCLIP)))
    {
        while(numspechit--)
        {
            // see if the line was crossed
            ld = spechit[numspechit];
            side = P_PointOnLineSide (thing->x, thing->y, ld);
            oldside = P_PointOnLineSide (oldx, oldy, ld);
            if(side != oldside)
            {
                if(ld->special & MLU_CROSS)
                    P_UseSpecialLine(thing, ld, oldside);
            }
        }
    }
    
    return true;
}

//
// TELEPORT MOVE
//

//
// P_TeleportMove
//

dboolean P_TeleportMove(mobj_t* thing, fixed_t x, fixed_t y)
{
    int             bx;
    int             by;
    subsector_t*    newsubsec;
    fixed_t         bbox[4];
    
    tmthing = thing;
    tmflags = thing->flags;
    
    tmx = x;
    tmy = y;
    
    newsubsec = R_PointInSubsector(x, y);
    tmhitline = NULL;
    
    // The base floor/ceiling is from the subsector
    // that contains the point.
    // Any contacted lines the step closer together
    // will adjust them.
    tmfloorz = tmdropoffz = M_PointToZ(&newsubsec->sector->floorplane, x, y);
    tmceilingz = M_PointToZ(&newsubsec->sector->ceilingplane, x, y);
    
    D_IncValidCount();
    numspechit = 0;
    
    P_BlockMapBox(bbox, x, y, tmthing);
    
    for(bx = bbox[BOXLEFT]; bx <= bbox[BOXRIGHT]; bx++)
    {
        for(by = bbox[BOXBOTTOM]; by <= bbox[BOXTOP]; by++)
        {
            // [d64] do stomping in actual teleport function
            if(!P_BlockThingsIterator(bx, by, PIT_CheckThing))
                return false;
        }
    }
            
    // the move is ok,
    // so link the thing into its new position
    P_UnsetThingPosition(thing);
            
    thing->floorz = tmfloorz;
    thing->ceilingz = tmceilingz;
    thing->x = x;
    thing->y = y;
            
    P_SetThingPosition(thing);
            
    return true;
}


//
// P_ThingHeightClip
// Takes a valid thing and adjusts the thing->floorz,
// thing->ceilingz, and possibly thing->z.
// This is called for all nearby monsters
// whenever a sector changes height.
// If the thing doesn't fit,
// the z will be set to the lowest value
// and false will be returned.
//

static dboolean P_ThingHeightClip(mobj_t* thing)
{
    dboolean            onfloor;
    
    onfloor = (thing->z == thing->floorz);
    
    P_CheckPosition(thing, thing->x, thing->y);
    // what about stranding a monster partially off an edge?
    
    thing->floorz = tmfloorz;
    thing->ceilingz = tmceilingz;
    
    if(onfloor)
        thing->z = thing->floorz;
    else
    {
        // don't adjust a floating monster unless forced to
        if(thing->z + thing->height > thing->ceilingz)
            thing->z = thing->ceilingz - thing->height;
    }
    
    if(thing->ceilingz - thing->floorz < thing->height)
        return false;
    
    return true;
}

//
// PIT_CheckMobjZ
//

mobj_t *onmobj;

static dboolean PIT_CheckMobjZ(mobj_t *thing)
{
    if(!(thing->flags & (MF_SOLID|MF_SPECIAL|MF_SHOOTABLE)))
        return true;    // Can't hit thing

    if(P_CheckThingCollision(thing))
        return true;

    if(thing == tmthing)
        return true;    // Don't clip against self

    if(tmthing->z > thing->z + thing->height)
        return true;

    else if(tmthing->z + tmthing->height < thing->z)
        return true;    // under thing

    if(thing->flags & MF_SOLID)
        onmobj = thing;

    return(!(thing->flags & MF_SOLID));
}

//
// P_CheckOnMobj
//

void P_ZMovement(mobj_t* mo, dboolean checkmissile);

mobj_t *P_CheckOnMobj(mobj_t *thing)
{
    int	bx, by;
    subsector_t *newsubsec;
    fixed_t x;
    fixed_t y;
    mobj_t oldmo;
    fixed_t bbox[4];
    
    x = thing->x;
    y = thing->y;
    tmthing = thing;
    tmflags = thing->flags;
    oldmo = *thing; // save the old mobj before the fake zmovement
    P_ZMovement(tmthing, false);
    
    tmx = x;
    tmy = y;
    
    newsubsec = R_PointInSubsector(x, y);
    tmhitline = NULL;
    
    //
    // the base floor / ceiling is from the subsector that contains the
    // point.  Any contacted lines the step closer together will adjust them
    //
    tmfloorz = tmdropoffz = M_PointToZ(&newsubsec->sector->floorplane, x, y);
    tmceilingz = M_PointToZ(&newsubsec->sector->ceilingplane, x, y);
    
    validcount++;
    numspechit = 0;
    
    if ( tmflags & MF_NOCLIP )
        return NULL;
    
    //
    // check things first, possibly picking things up
    // the bounding box is extended by MAXRADIUS because mobj_ts are grouped
    // into mapblocks based on their origin point, and can overlap into adjacent
    // blocks by up to MAXRADIUS units
    //
    P_BlockMapBox(bbox, x, y, tmthing);
    
    for(bx = bbox[BOXLEFT]; bx <= bbox[BOXRIGHT]; bx++)
    {
        for(by = bbox[BOXBOTTOM]; by <= bbox[BOXTOP]; by++)
        {
            if(!P_BlockThingsIterator(bx, by, PIT_CheckMobjZ))
            {
                *tmthing = oldmo;
                return onmobj;
            }
        }
    }

    *tmthing = oldmo;
    return NULL;
}



//
// SLIDE MOVE
// Allows the player to slide along any angled walls.
//

fixed_t         bestslidefrac;
line_t*         bestslideline;

mobj_t*         slidemo;

fixed_t         tmxmove;
fixed_t         tmymove;


//
// PTR_SlideTraverse
//

dboolean PTR_SlideTraverse (intercept_t* in)
{
    line_t* li = in->d.line;
    
    if(!(li->flags & ML_TWOSIDED))
    {
        if(P_PointOnLineSide (slidemo->x, slidemo->y, li))
        {
            // don't hit the back side
            return true;
        }
        
        goto isblocking;
    }
    
    // set openrange, opentop, openbottom
    P_LineOpening(li,
        trace.x + FixedMul(trace.dx, in->frac),
        trace.y + FixedMul(trace.dy, in->frac),
        MININT,
        MININT);
    
    if(openrange < slidemo->height)
        goto isblocking;                // doesn't fit
    
    if(opentop - slidemo->z < slidemo->height)
        goto isblocking;                // mobj is too high
    
    if(openbottom - slidemo->z > 24*FRACUNIT)
        goto isblocking;                // too big a step up
    
    // this line doesn't block movement
    return true;
    
    // the line does block movement,
    // see if it is closer than best so far
    
isblocking:
    
    if(in->frac < bestslidefrac)
    {
        bestslidefrac = in->frac;
        bestslideline = li;
    }
    
    return false;       // stop
}

//
// P_SlideMove
// The momx / momy move is bad, so try to slide
// along a wall.
// Find the first line hit, move flush to it,
// and slide along it
//
// This is a kludgy mess.
//

void P_SlideMove(mobj_t* mo)
{
    fixed_t leadx;
    fixed_t leady;
    fixed_t trailx;
    fixed_t traily;
    fixed_t newx;
    fixed_t newy;
    int     hitcount;
    line_t* ld;
    int     an1;
    int     an2;
    fixed_t xmove;
    fixed_t ymove;
    
    slidemo = mo;
    hitcount = 0;
    
retry:
    if(++hitcount == 3)
        goto stairstep;         // don't loop forever
    
    
    // trace along the three leading corners
    if(mo->momx > 0)
    {
        leadx = mo->x + mo->radius;
        trailx = mo->x - mo->radius;
    }
    else
    {
        leadx = mo->x - mo->radius;
        trailx = mo->x + mo->radius;
    }
    
    if(mo->momy > 0)
    {
        leady = mo->y + mo->radius;
        traily = mo->y - mo->radius;
    }
    else
    {
        leady = mo->y - mo->radius;
        traily = mo->y + mo->radius;
    }
    
    bestslidefrac = FRACUNIT+1;
    
    P_PathTraverse(leadx, leady, leadx + mo->momx, leady + mo->momy,
        PT_ADDLINES, PTR_SlideTraverse);
    P_PathTraverse(trailx, leady, trailx + mo->momx, leady + mo->momy,
        PT_ADDLINES, PTR_SlideTraverse);
    P_PathTraverse(leadx, traily, leadx + mo->momx, traily + mo->momy,
        PT_ADDLINES, PTR_SlideTraverse);
    
    // move up to the wall
    if(bestslidefrac == FRACUNIT+1)
    {
        // the move most have hit the middle, so stairstep
stairstep:
        xmove = 0;
        ymove = mo->momy;

        //
        // [kex] things could pose a potential issue so stop
        // momentum x or y if the move failed
        //
        if(!P_TryMove(mo, mo->x + xmove, mo->y + ymove))
        {
            xmove = mo->momx;
            ymove = 0;

            if(!P_TryMove(mo, mo->x + xmove, mo->y + ymove))
            {
                // [d64] set momx and momy to 0
                mo->momx = 0;
                mo->momy = 0;
            }
            else
                mo->momy = 0;
        }
        else
            mo->momx = 0;

        return;
    }
    
    // fudge a bit to make sure it doesn't hit
    bestslidefrac -= 0x800;
    if(bestslidefrac > 0)
    {
        newx = FixedMul(mo->momx, bestslidefrac);
        newy = FixedMul(mo->momy, bestslidefrac);
        
        if(!P_TryMove(mo, mo->x + newx, mo->y + newy))
        {
            bestslidefrac = FRACUNIT;

            // [d64] jump to hitslideline instead of stairstep
            goto hitslideline;
        }
    }
    
    // Now continue along the wall.
    // First calculate remainder.
    bestslidefrac = FRACUNIT - (bestslidefrac + 0x800);
    
    if(bestslidefrac > FRACUNIT)
        bestslidefrac = FRACUNIT;
    
    if(bestslidefrac <= 0)
        return;

    //
    // [d64] code below is loosely based on P_HitSlideLine
    //
hitslideline:

    ld = bestslideline;

    if(ld->slopetype == ST_HORIZONTAL)
        tmymove = 0;
    else
        tmymove = FixedMul(mo->momy, bestslidefrac);
    
    if(ld->slopetype == ST_VERTICAL)
        tmxmove = 0;
    else
        tmxmove = FixedMul(mo->momx, bestslidefrac);

    //
    // [d64] this new algorithm seems to reduce the chances
    // of boosting the player's speed when wall running
    //
    
    an1 = finecosine[ld->angle];
    an2 = finesine[ld->angle];
    
    if(P_PointOnLineSide(mo->x, mo->y, bestslideline))
    {
        //
        // [d64] same as deltaangle += ANG180 ?
        //
        an1 = -an1;
        an2 = -an2;
    }
    
    newx = FixedMul(tmxmove, an1);
    newy = FixedMul(tmymove, an2);
    
    mo->momx = FixedMul(newx + newy, an1);
    mo->momy = FixedMul(newx + newy, an2);

    if(P_CheckSlopeWalk(mo, &mo->momx, &mo->momy))
        mo->z = M_PointToZ(&mo->subsector->sector->floorplane, mo->x + mo->momx, mo->y + mo->momy);
    
    if(!P_TryMove(mo, mo->x + mo->momx, mo->y + mo->momy))
        goto retry;
}

//
// P_CheckSlopeWalk
//
// Adapted from Zdoom for Doom64EX
//

dboolean P_CheckSlopeWalk(mobj_t* thing, fixed_t* xmove, fixed_t* ymove)
{
    fixed_t destx;
    fixed_t desty;
    fixed_t t;
    plane_t* plane;
    fixed_t z;
    
    if(!(thing->flags & MF_GRAVITY))
        return false;
    
    plane = &thing->subsector->sector->floorplane;
    z = M_PointToZ(plane, thing->x, thing->y);
    
    if(z > thing->floorz + 4*FRACUNIT)
        return false;
    
    if(thing->z - z > FRACUNIT)
        return false;   // not on floor
    
    if((plane->a | plane->b) != 0)
    {
        destx = thing->x + *xmove;
        desty = thing->y + *ymove;

        t = -(M_DotProduct(plane->a, plane->b, plane->c, destx, desty, thing->z) + plane->d);

        if(t < 0)
        {
            // Desired location is behind (below) the plane
            // (i.e. Walking up the plane)
            if(plane->c >= -(dcos(ANG45)))
            {
                // Can't climb up slopes of ~45 degrees or more
                if(thing->flags & MF_NOCLIP)
                    return true;
                else
                {
                    *xmove = thing->momx = -plane->a * 4;
                    *ymove = thing->momy = -plane->b * 4;

                    return false;
                }
            }

            // Slide the desired location along the plane's normal
            // so that it lies on the plane's surface
            destx -= FixedMul(-plane->a, t);
            desty -= FixedMul(-plane->b, t);
            *xmove = (destx - thing->x);
            *ymove = (desty - thing->y);

            return true;
        }
        else if(t > 0)
        {
            // Desired location is in front of (above) the plane
            if(z == thing->z)
            {
                // mobj's current spot is on/in the plane, so walk down it
                // Same principle as walking up, except reversed
                destx += FixedMul(-plane->a, t);
                desty += FixedMul(-plane->b, t);
                *xmove = (destx - thing->x);
                *ymove = (desty - thing->y);

                return true;
            }
        }
    }

    return false;
}


//
// P_LineAttack
//
mobj_t*         linetarget;     // who got hit (or NULL)
mobj_t*         shootthing;
//fixed_t         aimfrac;

fixed_t         shootdirx;
fixed_t         shootdiry;
fixed_t         shootdirz;

//dboolean        shotsideline = false;
//dboolean        aimlaser = false;

// Height if not aiming up or down
// ???: use slope for monsters?
fixed_t         shootz;

int             la_damage;
fixed_t         attackrange;

fixed_t         aimslope;
fixed_t         aimpitch;

// slopes to top and bottom of target
extern fixed_t  topslope;
extern fixed_t  bottomslope;

// [kex]
extern fixed_t laserhit_x;
extern fixed_t laserhit_y;
extern fixed_t laserhit_z;


//
// PTR_AimTraverse
// Sets linetaget and aimslope when a target is aimed at.
//
dboolean PTR_AimTraverse(intercept_t* in)
{
    line_t* li;
    mobj_t* th;
    fixed_t linebottomslope;
    fixed_t linetopslope;
    fixed_t thingtopslope;
    fixed_t thingbottomslope;
    fixed_t dist;
    
    if(in->isaline)
    {
        li = in->d.line;
        
        // [kex] if a line was already hit, then ignore
        //if(!shotsideline)
            //aimfrac = in->frac;
        
        if(!(li->flags & ML_TWOSIDED))
            return false;               // stop
        
        // Crosses a two sided line.
        // A two sided line will restrict
        // the possible target ranges.
        P_LineOpening(li,
            trace.x + FixedMul(trace.dx, in->frac),
            trace.y + FixedMul(trace.dy, in->frac),
            MININT,
            MININT);
        
        if(openbottom >= opentop)
            return false;               // stop
        
        dist = FixedMul(attackrange, in->frac);
        
        if(li->frontsector->floorheight != li->backsector->floorheight)
        {
            linebottomslope = FixedDiv(openbottom - shootz , dist);
            if(linebottomslope > bottomslope)
                bottomslope = linebottomslope;
        }
        
        if(li->frontsector->ceilingheight != li->backsector->ceilingheight)
        {
            linetopslope = FixedDiv(opentop - shootz , dist);
            if(linetopslope < topslope)
                topslope = linetopslope;
        }

        // [kex] d64 bug fix in which lasers ignore side lines when getting aimfrac
        /*if(aimlaser && !shotsideline)
        {
            // check floor heights
            if(li->frontsector->floorheight != li->backsector->floorheight)
            {
                if(linebottomslope > aimslope)
                {
                    shotsideline = true;
                    return true;    // shot continues
                }
            }

            // check ceiling heights
            if(li->frontsector->ceilingheight != li->backsector->ceilingheight)
            {
                if(linetopslope < aimslope)
                {
                    shotsideline = true;
                    return true;    // shot continues
                }
            }
        }*/
        
        if(topslope <= bottomslope)
            return false;               // stop
        
        return true;                    // shot continues
    }
    
    // shoot a thing
    th = in->d.thing;
    if(th == shootthing)
        return true;                    // can't shoot self
    
    if(!(th->flags & MF_SHOOTABLE))
        return true;                    // corpse or something
    
    // check angles to see if the thing can be aimed at
    dist = FixedMul(attackrange, in->frac);
    thingtopslope = FixedDiv(th->z + th->height - shootz , dist);
    
    if(thingtopslope < bottomslope)
        return true;                    // shot over the thing
    
    thingbottomslope = FixedDiv(th->z - shootz, dist);
    
    if(thingbottomslope > topslope)
        return true;                    // shot under the thing
    
    // this thing can be hit!
    if(thingtopslope > topslope)
        thingtopslope = topslope;
    
    if(thingbottomslope < bottomslope)
        thingbottomslope = bottomslope;
    
    aimslope = (thingtopslope+thingbottomslope) >> 1;
    linetarget = th;
    //aimfrac = in->frac;
    
    return false;                       // don't go any farther
}

//
// PTR_ShootTraverse
//
dboolean PTR_ShootTraverse(intercept_t* in)
{
    fixed_t     x;
    fixed_t     y;
    fixed_t     z;
    fixed_t     frac;
    line_t*     li;
    mobj_t*     th;
    fixed_t     slope;
    fixed_t     dist;
    fixed_t     thingtopslope;
    fixed_t     thingbottomslope;
    dboolean    hitplane = false;
    int         lineside;
    sector_t*   sidesector;
    fixed_t     hitx;
    fixed_t     hity;
    fixed_t     hitz;
    fixed_t     fz;
    fixed_t     cz;
    
    if(in->isaline)
    {
        li = in->d.line;
        
        lineside = P_PointOnLineSide(shootthing->x, shootthing->y, li);
        sidesector = lineside ? li->backsector : li->frontsector;
        dist = FixedMul(attackrange, in->frac);
        hitx = trace.x + FixedMul(shootdirx, dist);
		hity = trace.y + FixedMul(shootdiry, dist);
        hitz = shootz + FixedMul(shootdirz, dist);

        if (li->flags & ML_TWOSIDED)
        {
            fixed_t ffz, fcz;
            fixed_t bfz, bcz;

            // crosses a two sided line
            P_LineOpening(li,
                trace.x + FixedMul(trace.dx, in->frac),
                trace.y + FixedMul(trace.dy, in->frac),
                MININT,
                MININT);

            ffz = M_PointToZ(&li->frontsector->floorplane, hitx, hitz);
            fcz = M_PointToZ(&li->frontsector->ceilingplane, hitx, hitz);
            bfz = M_PointToZ(&li->backsector->floorplane, hitx, hitz);
            bcz = M_PointToZ(&li->backsector->ceilingplane, hitx, hitz);
        
            if((ffz == bfz 
                || (slope = FixedDiv(openbottom - shootz , dist)) <= aimslope) 
                && (fcz == bcz 
                || (slope = FixedDiv(opentop - shootz , dist)) >= aimslope))
                return true;      // shot continues

            fz = lineside ? bfz : ffz;
            cz = lineside ? bcz : fcz;
        }
        else
        {
            fz = M_PointToZ(&sidesector->floorplane, hitx, hitz);
            cz = M_PointToZ(&sidesector->ceilingplane, hitx, hitz);
        }

        //
        // [kex] - hit ceiling/floor
        // set position based on intersection
        // 
        if(hitz <= fz || hitz >= cz)
        {
            plane_t* plane;
            fixed_t den;

            if(hitz <= fz)
                plane = &sidesector->floorplane;
            else
                plane = &sidesector->ceilingplane;

            den = M_DotProduct(plane->a, plane->b, plane->c, shootdirx, shootdiry, shootdirz);

            if(den != 0)
            {
                fixed_t hitdist;
                fixed_t num = M_DotProduct(
                    plane->a, plane->b,
                    plane->c, trace.x,
                    trace.y, shootz) + plane->d;

                hitdist = FixedDiv(-num, den);

                frac = FixedDiv(hitdist, attackrange);
                x = trace.x + FixedMul(shootdirx, hitdist);
                y = trace.y + FixedMul(shootdiry, hitdist);
                z = shootz + FixedMul(shootdirz, hitdist);

                hitplane = true;
            }
        }
        else
        {
            // hit line
            // position a bit closer
            frac = in->frac - FixedDiv(4*FRACUNIT, attackrange);
            x = trace.x + FixedMul(trace.dx, frac);
            y = trace.y + FixedMul(trace.dy, frac);
            z = shootz + FixedMul(aimslope, FixedMul(frac, attackrange));
        }

        if(!hitplane && li->special & MLU_SHOOT)
            P_UseSpecialLine(shootthing, li, lineside);
        
        //don't shoot blank textures
        if(sides[li->sidenum[0]].midtexture == 1
            || sides[li->sidenum[0]].toptexture == 1
            || sides[li->sidenum[0]].bottomtexture == 1)
            return false;
        
        if(li->frontsector->ceilingpic == skyflatnum)
        {
            // don't shoot the sky!
            if(z + FRACUNIT >= M_PointToZ(&li->frontsector->ceilingplane, x, y))
                return false;
            
            // it's a sky hack wall
            if(li->backsector && li->backsector->ceilingpic == skyflatnum
                && M_PointToZ(&li->backsector->ceilingplane, x, y) < z + FRACUNIT)
                return false;
        }

        if(attackrange == LASERRANGE)
        {
            laserhit_x = x;
            laserhit_y = y;
            laserhit_z = z;
        }
        else
        {
            // Spawn bullet puffs.
            P_SpawnPuff(x, y, z);
        }
        
        // don't go any farther
        return false;
    }
    
    // shoot a thing
    th = in->d.thing;
    if(th == shootthing)
        return true;            // can't shoot self
    
    if(!(th->flags & MF_SHOOTABLE))
        return true;            // corpse or something
    
    // check angles to see if the thing can be aimed at
    dist = FixedMul(attackrange, in->frac);
    thingtopslope = FixedDiv(th->z+th->height - shootz , dist);
    
    if (thingtopslope < aimslope)
        return true;            // shot over the thing
    
    thingbottomslope = FixedDiv(th->z - shootz, dist);
    
    if (thingbottomslope > aimslope)
        return true;            // shot under the thing
    
    
    // hit thing
    // position a bit closer
    frac = in->frac - FixedDiv(10*FRACUNIT,attackrange);
    
    x = trace.x + FixedMul(trace.dx, frac);
    y = trace.y + FixedMul(trace.dy, frac);
    z = shootz + FixedMul(aimslope, FixedMul(frac, attackrange));
    
    // Spawn bullet puffs or blod spots,
    // depending on target type.
    //
    // [kex] if something was hit with the laser then save the
    // hit location for later
    //
    if(attackrange == LASERRANGE)
    {
        laserhit_x = x;
        laserhit_y = y;
        laserhit_z = z;
    }
    else if(in->d.thing->flags & MF_NOBLOOD)
        P_SpawnPuff(x, y, z);
    else
        P_SpawnBlood(x, y, z, la_damage);
    
    if(la_damage)
        P_DamageMobj(th, shootthing, shootthing, la_damage);
    
    // don't go any farther
    return false;
    
}


//
// P_AimLineAttack
//

fixed_t P_AimLineAttack(mobj_t*	t1, angle_t angle, angle_t pitch, fixed_t zheight, fixed_t distance)
{
    int flags;
    fixed_t x2;
    fixed_t y2;
    fixed_t lookdir = 0;
    
    angle >>= ANGLETOFINESHIFT;
    shootthing = t1;
    
    x2 = t1->x + F2INT(distance)*finecosine[angle];
    y2 = t1->y + F2INT(distance)*finesine[angle];

    // [d64] new argument for shoot height
    if(!zheight)
        shootz = t1->z + (t1->height>>1) + 12*FRACUNIT;
    else
        shootz = t1->z + zheight;
    
    // can't shoot outside view angles
    // [d64] use 120 instead of 100
    topslope        = 120*FRACUNIT/160;
    bottomslope     = -120*FRACUNIT/160;
    
    attackrange	    = distance;
    linetarget	    = NULL;
    //aimfrac		    = 0;
    //shotsideline    = false;
    //aimlaser        = false;
    flags           = PT_ADDLINES|PT_ADDTHINGS;

    if(t1->player)
    {
        lookdir = dcos(D_abs(pitch - ANG90));
        
        // [kex] set aimslope for special purposes
        /*if(distance == LASERRANGE)
        {
            aimslope = lookdir;
            aimlaser = true;
        }*/

        // [kex] check for autoaim option. skip if bfg spray
        if(distance != (ATTACKRANGE + 1))
        {
            if(!t1->player->autoaim &&
                distance != MELEERANGE && distance != (MELEERANGE + 1))
                flags &= ~PT_ADDTHINGS;
        }
    }
    
    P_PathTraverse(t1->x, t1->y, x2, y2, flags, PTR_AimTraverse);
    
    if(linetarget)
        return aimslope;

    // [kex] adjust slope based on view pitch
    else if(t1->player)
    {
        aimslope = lookdir;
        
        if(D_abs(aimslope) > 0x40)
            return aimslope;
    }
    
    return 0;
}


//
// P_LineAttack
// If damage == 0, it is just a test trace
// that will leave linetarget set.
//

void P_LineAttack(mobj_t* t1, angle_t angle, angle_t pitch, fixed_t distance, fixed_t slope, int damage)
{
    fixed_t x2;
    fixed_t y2;
    
    angle >>= ANGLETOFINESHIFT;
    shootthing = t1;
    la_damage = damage;
    x2 = t1->x + F2INT(distance)*finecosine[angle];
    y2 = t1->y + F2INT(distance)*finesine[angle];
    shootz = t1->z + (t1->height>>1) + 12*FRACUNIT; // [d64] changed from 8 to 12
    attackrange = distance;
    aimslope = slope;
    aimpitch = dcos(pitch);

    //
    // [kex] - stuff for plane hit detection
    //
    shootdirx   = FixedMul(aimpitch, finecosine[angle]);
    shootdiry   = FixedMul(aimpitch, finesine[angle]);
    shootdirz   = dsin(pitch);
    laserhit_x  = t1->x;
    laserhit_y  = t1->y;
    laserhit_z  = t1->z;
    
    P_PathTraverse(t1->x, t1->y, x2, y2, PT_ADDLINES|PT_ADDTHINGS, PTR_ShootTraverse);
}

//
// USE LINES
//

static mobj_t*  usething = NULL;
static dboolean usecontext = false;
static dboolean displaycontext = false;
line_t* contextline = NULL;

//
// P_CheckUseHeight
//

static dboolean P_CheckUseHeight(line_t *line, mobj_t *thing)
{
    fixed_t check = 0;
    
    if(!(line->flags & ML_SWITCHX02 ||
        line->flags & ML_SWITCHX04 ||
        line->flags & ML_SWITCHX08))
        return true;	// ignore non-switches
    
    if(line->flags & ML_CHECKFLOORHEIGHT)
    {
        if(line->flags & ML_TWOSIDED)
            check = (line->backsector->floorheight + sides[line->sidenum[0]].rowoffset) - (32*FRACUNIT);
        else
            check = (line->frontsector->floorheight + sides[line->sidenum[0]].rowoffset) + (32*FRACUNIT);
    }
    else if(line->flags & ML_TWOSIDED)
        check = (line->backsector->ceilingheight + sides[line->sidenum[0]].rowoffset) + (32*FRACUNIT);
    else
        return true;
    
    if(!(check < thing->z))
    {
        if((thing->z + thing->height) < check)
            return false;
    }
    else
        return false;
    
    return true;
}


//
// PTR_UseTraverse
//

dboolean PTR_UseTraverse(intercept_t* in)
{
    if(!in->d.line->special)
    {
        P_LineOpening(in->d.line,
            trace.x + FixedMul(trace.dx, in->frac),
            trace.y + FixedMul(trace.dy, in->frac),
            MININT,
            MININT);
        
        if(openrange <= 0)
        {
            //
            // [kex] don't spam oof sound if usecontext is on
            //
            if(!usecontext)
                S_StartSound(usething, sfx_noway);

            return false;	// can't use through a wall
        }
        
        
        return true;	// keep checking
    }
    
    if(!(in->d.line->special & MLU_USE) || !P_CheckUseHeight(in->d.line, usething))
        return false;
    
    //
    // [kex] don't trigger special if usecontext is on
    //
    if(!usecontext)
    {
        P_UseSpecialLine(usething, in->d.line,
            (P_PointOnLineSide(usething->x, usething->y, in->d.line) == 1));
    }

    displaycontext = true;
    contextline = in->d.line;
    
    // can't use for than one special line in a row
    return false;
}

//
// P_UseLines
// Looks for special lines in front of the player to activate.
//

dboolean P_UseLines(player_t *player, dboolean showcontext) 
{
    int		angle;
    fixed_t	x1;
    fixed_t	y1;
    fixed_t	x2;
    fixed_t	y2;
    
    usething = player->mo;

    //
    // [kex] use context stuff
    //
    usecontext      = showcontext;
    displaycontext  = false;
    contextline     = NULL;
    
    angle = player->mo->angle >> ANGLETOFINESHIFT;
    
    x1 = player->mo->x;
    y1 = player->mo->y;
    x2 = x1 + F2INT(USERANGE)*finecosine[angle];
    y2 = y1 + F2INT(USERANGE)*finesine[angle];
    
    P_PathTraverse(x1, y1, x2, y2, PT_ADDLINES, PTR_UseTraverse);

    return displaycontext;
}


//
// RADIUS ATTACK
//

mobj_t*         bombsource;
mobj_t*         bombspot;
int             bombdamage;


//
// PIT_RadiusAttack
// "bombsource" is the creature that caused the explosion at "bombspot"
//

dboolean PIT_RadiusAttack(mobj_t* thing)
{
    fixed_t     dx;
    fixed_t     dy;
    fixed_t     dist;
    
    if(!(thing->flags & MF_SHOOTABLE))
        return true;
    
    // Boss cyborg take no damage from concussion.
    if(thing->type == MT_CYBORG)
        return true;
    
    if(thing->type == MT_SKULL || thing->type == MT_PAIN)
    {
        if(bombsource && bombsource->type == MT_SKULL)
            return true;
    }
    
    dx = D_abs(thing->x - bombspot->x);
    dy = D_abs(thing->y - bombspot->y);
    
    dist = dx>dy ? dx : dy;
    dist = F2INT(dist - thing->radius);
    
    if(dist < 0)
        dist = 0;
    
    if(dist >= bombdamage)
        return true;    // out of range
    
    // must be in direct path
    if(P_CheckSight(thing, bombspot))
        P_DamageMobj(thing, bombspot, bombsource, bombdamage - dist);
    
    return true;
}


//
// P_RadiusAttack
// Source is the creature that caused the explosion at spot.
//
void P_RadiusAttack(mobj_t* spot, mobj_t* source, int damage)
{
    int         x;
    int         y;
    
    int         xl;
    int         xh;
    int         yl;
    int         yh;
    
    fixed_t     dist;
    
    dist = INT2F(damage);
    yh = (spot->y + dist - bmaporgy)>>MAPBLOCKSHIFT;
    yl = (spot->y - dist - bmaporgy)>>MAPBLOCKSHIFT;
    xh = (spot->x + dist - bmaporgx)>>MAPBLOCKSHIFT;
    xl = (spot->x - dist - bmaporgx)>>MAPBLOCKSHIFT;
    bombspot = spot;
    bombsource = source;
    bombdamage = damage;
    
    for (y=yl ; y<=yh ; y++)
        for (x=xl ; x<=xh ; x++)
            P_BlockThingsIterator (x, y, PIT_RadiusAttack );
}

//
// SECTOR HEIGHT CHANGING
// After modifying a sectors floor or ceiling height,
// call this routine to adjust the positions
// of all things that touch the sector.
//
// If anything doesn't fit anymore, true will be returned.
// If crunch is true, they will take damage
//  as they are being crushed.
// If Crunch is false, you should set the sector height back
//  the way it was and call P_ChangeSector again
//  to undo the changes.
//
static int crushchange;
static dboolean nofit;


//
// PIT_ChangeSector
//
dboolean PIT_ChangeSector(mobj_t* thing)
{
    mobj_t* mo;

    if(P_ThingHeightClip(thing))
    {
        // keep checking
        return true;
    }
    
    // crunch bodies to giblets
    if(thing->health <= 0 && thing->tics == -1)
    {
        if(thing->state != &states[S_498])
        {
            P_SetMobjState(thing, S_498);
            S_StartSound(thing, sfx_slop);
        }
        
        // keep checking
        return true;
    }
    
    // crunch dropped items
    if(thing->flags & MF_DROPPED)
    {
        P_RemoveMobj (thing);
        
        // keep checking
        return true;
    }
    
    if(!(thing->flags & MF_SHOOTABLE))
    {
        // assume it is bloody gibs or something
        return true;
    }
    
    nofit = true;
    
    //
    // [d64] insta-kill thing if sector type is 666
    //
    if(crushchange == 2)
    {
        P_DamageMobj(thing, NULL, NULL, 99999);
        return true;
    }
    else
    {
        if(crushchange && !(leveltime & 3))
        {
            P_DamageMobj(thing, NULL, NULL, 10);
            
            // spray blood in a random direction
            mo = P_SpawnMobj(thing->x, thing->y, thing->z + thing->height / 2, MT_BLOOD);
            
            mo->momx = P_RandomShift(12);
            mo->momy = P_RandomShift(12);
        }
    }
    
    // keep checking (crush other things)
    return true;
}



//
// P_ChangeSector
//
dboolean P_ChangeSector(sector_t* sector, dboolean crunch)
{
    int x;
    int y;
    
    nofit = false;
    crushchange = crunch;
    
    // [d64] handle special case if sector's special is 666
    if(sector->special == 666)
        crushchange = 2;
    
    // re-check heights for all things near the moving sector
    for(x = sector->blockbox[BOXLEFT]; x <= sector->blockbox[BOXRIGHT]; x++)
        for(y = sector->blockbox[BOXBOTTOM]; y <= sector->blockbox[BOXTOP]; y++)
            P_BlockThingsIterator (x, y, PIT_ChangeSector);
        return nofit;
}

//
// CHASE CAMERA CLIP/MOVEMENT
//

mobj_t* tmcamera;

//
// PTR_ChaseCamTraverse
//

static dboolean PTR_ChaseCamTraverse(intercept_t* in)
{
    fixed_t x;
    fixed_t y;
    fixed_t frac;

    if(in->isaline)
    {
        side_t* side;
        line_t* li;

        li = in->d.line;

        if(li->flags & ML_TWOSIDED)
        {
            if(!(li->flags & (ML_DRAWMIDTEXTURE|ML_BLOCKING)))
            {
                sector_t* front;
                sector_t* back;

                P_LineOpening(li,
                    trace.x + FixedMul(trace.dx, in->frac),
                    trace.y + FixedMul(trace.dy, in->frac),
                    MININT,
                    MININT);

                front = li->frontsector;
                back = li->backsector;

                if(front->ceilingheight != front->floorheight &&
                    back->ceilingheight != back->floorheight &&
                    back->floorheight < front->ceilingheight &&
                    front->ceilingheight > back->floorheight &&
                    tmcamera->z >= openbottom)
                    return true; // continue
            }
        }

        side = &sides[li->sidenum[0]];

        if(side->midtexture == 1 || side->toptexture == 1 || side->bottomtexture == 1)
            return true; // continue

        frac = in->frac - FixedDiv(6*FRACUNIT, MISSILERANGE);
        x = trace.x + FixedMul(trace.dx, frac);
        y = trace.y + FixedMul(trace.dy, frac);

        tmcamera->x = x;
        tmcamera->y = y;

        return false; // don't go any farther
    }

    return true;
}

//
// P_CheckChaseCamPosition
//

void P_CheckChaseCamPosition(mobj_t* target, mobj_t* camera, fixed_t x, fixed_t y)
{
    tmcamera = camera;

    if(P_PathTraverse(target->x, target->y, x, y, PT_ADDLINES, PTR_ChaseCamTraverse))
    {
        camera->x = x;
        camera->y = y;
    }
}

