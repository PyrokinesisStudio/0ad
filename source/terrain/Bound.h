//-----------------------------------------------------------
//
// Name:		Bound.h
// Last Update: 25/11/03
// Author:		Rich Cross
// Contact:		rich@0ad.wildfiregames.com
//
// Description: Basic axis aligned bounding box class
//
//-----------------------------------------------------------

#ifndef _BOUND_H
#define _BOUND_H

// necessary includes
#include "Vector3D.h"

class CBound
{
public:
    CBound() {}
    CBound(const CVector3D& min,const CVector3D& max) {
		m_Data[0]=min; m_Data[1]=max;
	}

	CVector3D& operator[](int index) {	return m_Data[index]; }
    const CVector3D& operator[](int index) const { return m_Data[index]; }

	void SetEmpty();

	CBound& operator+=(const CBound& b);
	CBound& operator+=(const CVector3D& pt);

    bool RayIntersect(const CVector3D& origin,const CVector3D& dir,float& tmin,float& tmax) const;

	float GetVolume() const {
		CVector3D v=m_Data[1]-m_Data[0];
		return v.X*v.Y*v.Z;
	}


private:
    CVector3D m_Data[2];
};
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#endif
