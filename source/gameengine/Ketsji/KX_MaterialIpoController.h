
/** \file KX_MaterialIpoController.h
 *  \ingroup ketsji
 */

#pragma once


#include "KX_IInterpolator.h"
#include "SG_Controller.h"
#include "SG_Node.h"

class RAS_IPolyMaterial;

class KX_MaterialIpoController : public SG_Controller
{
public:
  MT_Vector4 m_rgba;
  MT_Vector3 m_specrgb;
  float m_hard;
  float m_spec;
  float m_ref;
  float m_emit;
  float m_ambient;
  float m_alpha;
  float m_specAlpha;

private:
  T_InterpolatorList	m_interpolators;

  RAS_IPolyMaterial *m_material;

public:
  KX_MaterialIpoController(RAS_IPolyMaterial *polymat) : 
    m_material(polymat)
  {}
  virtual ~KX_MaterialIpoController();
  virtual SG_Controller*	GetReplica(class SG_Node* destnode);
  virtual bool Update(double time);

  void AddInterpolator(KX_IInterpolator* interp);
};

