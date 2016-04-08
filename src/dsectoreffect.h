#ifndef __DSECTOREFFECT_H__
#define __DSECTOREFFECT_H__

#include "dthinker.h"
#include "r_defs.h"

class DSectorEffect : public DThinker
{
	DECLARE_CLASS (DSectorEffect, DThinker)
public:
	DSectorEffect (sector_t *sector);

	void Serialize (FArchive &arc);
	void Destroy();

	sector_t *GetSector() const { return m_Sector; }

protected:
	DSectorEffect ();
	sector_t *m_Sector;
};

class DMover : public DSectorEffect
{
	DECLARE_CLASS (DMover, DSectorEffect)
	HAS_OBJECT_POINTERS
public:
	DMover (sector_t *sector);
protected:
	enum EResult { ok, crushed, pastdest };
	TObjPtr<DInterpolation> interpolation;
private:
	bool MoveAttached(int crush, double move, int floorOrCeiling, bool resetfailed);
protected:
	DMover ();
	void Serialize (FArchive &arc);
	void Destroy();
	void StopInterpolation(bool force = false);
	EResult MoveFloor(double speed, double dest, int crush, int direction, bool hexencrush);
	EResult MoveCeiling(double speed, double dest, int crush, int direction, bool hexencrush);

	inline EResult MoveFloor(double speed, double dest, int direction)
	{
		return MoveFloor(speed, dest, -1, direction, false);
	}

	inline EResult MoveCeiling(double speed, double dest, int direction)
	{
		return MoveCeiling(speed, dest, -1, direction, false);
	}

};

class DMovingFloor : public DMover
{
	DECLARE_CLASS (DMovingFloor, DMover)
public:
	DMovingFloor (sector_t *sector);
protected:
	DMovingFloor ();
};

class DMovingCeiling : public DMover
{
	DECLARE_CLASS (DMovingCeiling, DMover)
public:
	DMovingCeiling (sector_t *sector);
protected:
	DMovingCeiling ();
};

#endif //__DSECTOREFFECT_H__
