/*
savenewvertex: particle stoped because it decayed
               save the daughter particles and the
               decay vertex.


*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <hddm_s.h>
#include <geant3.h>

extern s_HDDM_t* thisInputEvent;

void SaveNewVertex(int kcase, int Npart, float *gkin, 
		   float vertex[3], float tofg, int ipart){


  s_Vertices_t* verts;
  int VertexCount;
  s_Vertex_t* vert;
  s_Products_t* ps;
  s_Origin_t* or;
  int i = 0;

  // get pointer to all verteces
  verts = thisInputEvent->physicsEvents->in[0].reactions->in[0].vertices;
  VertexCount = verts->mult;
  vert = &verts->in[VertexCount];
  verts->mult++;
  
  // make space for the vertex coordinates
  or = make_s_Origin();
  verts->in[VertexCount].origin = or;
  or->vx = vertex[0];
  or->vy = vertex[1];
  or->vz = vertex[2];
  or->t = tofg;

  // make space for the particles at this vertex
  ps = make_s_Products(Npart);
  verts->in[VertexCount].products = ps;
  ps->mult = Npart;
  for (i = 0;i<Npart;i++){
    
    ps->in[i].momentum = make_s_Momentum();
    ps->in[i].momentum->px = gkin[i*5+0];
    ps->in[i].momentum->py = gkin[i*5+1];
    ps->in[i].momentum->pz = gkin[i*5+2];
    ps->in[i].momentum->E  = gkin[i*5+3];
    ps->in[i].type = gkin[i*5+4];
    ps->in[i].parentid = ipart;
    ps->in[i].id = i;
    ps->in[i].mech = kcase;
    ps->in[i].decayVertex = VertexCount;

  }
    
}



/* entry point from fortran */

void savenewvertex_ (int *kcase, int *N, float* gkin, 
		     float* vertex, float* tofg, int* ipart) {

  SaveNewVertex(*kcase, *N, gkin, vertex, *tofg, *ipart);

}

