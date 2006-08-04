#include "meep-ctl.hpp"
#include <ctlgeom.h>

using namespace ctlio;

#define master_printf meep::master_printf
#define MTS material_type_struct

static meep::ndim dim = meep::D3;

/***********************************************************************/

void set_dimensions(int dims)
{
  if (dims == CYLINDRICAL) {
    dimensions = 2;
    dim = meep::Dcyl;
  }
  else {
    dimensions = dims;
    dim = meep::ndim(dims - 1);
  }
}

vector3 vec_to_vector3(const meep::vec &v)
{
  vector3 v3;
  
  switch (v.dim) {
  case meep::D1:
    v3.x = 0;
    v3.y = 0;
    v3.z = v.z();
    break;
  case meep::D2:
    v3.x = v.x();
    v3.y = v.y();
    v3.z = 0;
    break;
  case meep::D3:
    v3.x = v.x();
    v3.y = v.y();
    v3.z = v.z();
    break;
  case meep::Dcyl:
    v3.x = v.r();
    v3.y = 0;
    v3.z = v.z();
    break;
  }
  return v3;
}

meep::vec vector3_to_vec(const vector3 v3)
{
  switch (dim) {
  case meep::D1:
    return meep::vec(v3.z);
  case meep::D2:
    return meep::vec(v3.x, v3.y);
  case meep::D3:
    return meep::vec(v3.x, v3.y, v3.z);
  case meep::Dcyl:
    return meep::veccyl(v3.x, v3.z);
  default:
    meep::abort("unknown dimensionality in vector3_to_vec");
  }
}

static geom_box gv2box(const meep::geometric_volume &gv)
{
  geom_box box;
  box.low = vec_to_vector3(gv.get_min_corner());
  box.high = vec_to_vector3(gv.get_max_corner());
  return box;
}

/***********************************************************************/

class geom_epsilon : public meep::material_function {
  geometric_object_list geometry;
  geom_box_tree geometry_tree;
  geom_box_tree restricted_tree;
  
public:
  geom_epsilon(geometric_object_list g,
	       const meep::geometric_volume &gv);
  virtual ~geom_epsilon();
  
  virtual void set_volume(const meep::geometric_volume &gv);
  virtual void unset_volume(void);
  virtual double eps(const meep::vec &r);
  virtual bool has_chi3();
  virtual double chi3(const meep::vec &r);
  virtual bool has_chi2();
  virtual double chi2(const meep::vec &r);

  virtual meep::vec normal_vector(const meep::geometric_volume &gv);
  virtual void meaneps(double &meps, double &minveps, meep::vec &normal,
		       const meep::geometric_volume &gv, 
		       double tol, int maxeval);

  void fallback_meaneps(double &meps, double &minveps,
			const meep::geometric_volume &gv,
			double tol, int maxeval);

  virtual double sigma(const meep::vec &r);
  void add_polarizabilities(meep::structure *s);
};

geom_epsilon::geom_epsilon(geometric_object_list g,
			   const meep::geometric_volume &gv)
{
  geometry = g; // don't bother making a copy, only used in one place
  
  if (meep::am_master()) {
    for (int i = 0; i < geometry.num_items; ++i) {
      display_geometric_object_info(5, geometry.items[i]);
      
      if (geometry.items[i].material.which_subclass 
	  == MTS::DIELECTRIC)
	printf("%*sdielectric constant epsilon = %g\n",
	       5 + 5, "",
	       geometry.items[i].material.
	       subclass.dielectric_data->epsilon);
    }
  }
  
  geom_fix_objects0(geometry);
  geom_box box = gv2box(gv);
  geometry_tree = create_geom_box_tree0(geometry, box);
  if (verbose && meep::am_master()) {
    printf("Geometric-object bounding-box tree:\n");
    display_geom_box_tree(5, geometry_tree);
    
    int tree_depth, tree_nobjects;
    geom_box_tree_stats(geometry_tree, &tree_depth, &tree_nobjects);
    master_printf("Geometric object tree has depth %d "
		  "and %d object nodes (vs. %d actual objects)\n",
		  tree_depth, tree_nobjects, geometry.num_items);
  }
  
  restricted_tree = geometry_tree;
}

geom_epsilon::~geom_epsilon()
{
  unset_volume();
  destroy_geom_box_tree(geometry_tree);
}

void geom_epsilon::unset_volume(void)
{
  if (restricted_tree != geometry_tree) {
    destroy_geom_box_tree(restricted_tree);
    restricted_tree = geometry_tree;
  }
}

void geom_epsilon::set_volume(const meep::geometric_volume &gv)
{
  unset_volume();
  
  geom_box box = gv2box(gv);
  restricted_tree = create_geom_box_tree0(geometry, box);
}

static material_type eval_material_func(function material_func, vector3 p)
{
  SCM pscm = ctl_convert_vector3_to_scm(p);
  material_type material;
  SCM mo;
  
  mo = gh_call1(material_func, pscm);
  material_type_input(mo, &material);
  
  while (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material_type m;
    
    mo = gh_call1(material.subclass.
		  material_function_data->material_func,
		  pscm);
    material_type_input(mo, &m);
    material_type_destroy(material);
    material = m;
  }
  
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material_type_copy(&default_material, &material);
  }
  CK(material.which_subclass != MTS::MATERIAL_FUNCTION,
     "infinite loop in material functions");
  
  return material;
}

static int variable_material(int which_subclass)
{
     return (which_subclass == MTS::MATERIAL_FUNCTION);
}

static void material_eps(material_type material, double &eps, double &eps_inv) {
  switch (material.which_subclass) {
  case MTS::DIELECTRIC:
    eps = material.subclass.dielectric_data->epsilon;
    eps_inv = 1.0 / eps;
    break;
  case MTS::PERFECT_METAL:
    eps = -meep::infinity;
    eps_inv = -0.0;
    break;
  default:
    meep::abort("unknown material type");
  }
}

double geom_epsilon::eps(const meep::vec &r)
{
  double eps = 1.0, eps_inv;
  vector3 p = vec_to_vector3(r);

#ifdef DEBUG
  if (p.x < restricted_tree->b.low.x ||
      p.y < restricted_tree->b.low.y ||
      p.z < restricted_tree->b.low.z ||
      p.x > restricted_tree->b.high.x ||
      p.y > restricted_tree->b.high.y ||
      p.z > restricted_tree->b.high.z)
    meep::abort("invalid point (%g,%g,%g)\n", p.x,p.y,p.z);
#endif

  boolean inobject;
  material_type material =
    material_of_unshifted_point_in_tree_inobject(p, restricted_tree, &inobject);
  
  int destroy_material = 0;
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material = default_material;
  }
  if (variable_material(material.which_subclass)) {
    material = eval_material_func(material.subclass.
				  material_function_data->material_func,
				  p);
    destroy_material = 1;
  }

  material_eps(material, eps, eps_inv);  
  
  if (destroy_material)
    material_type_destroy(material);
  
  return eps;
}

/* Find frontmost object in gv, along with the constant material behind it.
   Returns false if material behind the object is not constant.
   
   Requires moderately horrifying logic to figure things out properly,
   stolen from MPB. */
static bool get_front_object(const meep::geometric_volume &gv,
			     geom_box_tree geometry_tree,
			     vector3 &pcenter,
			     const geometric_object **o_front,
			     vector3 &shiftby_front,
			     material_type &mat_front,
			     material_type &mat_behind) {
  vector3 p;
  const geometric_object *o1 = 0, *o2 = 0;
  vector3 shiftby1, shiftby2;
  geom_box pixel;
  material_type mat1, mat2;
  int id1 = -1, id2 = -1;
  const int num_neighbors[3] = { 3, 5, 9 };
  const int neighbors[3][9][3] = {
    { {0,0,0}, {-1,0,0}, {1,0,0},
      {0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0} },
    { {0,0,0},
      {-1,-1,0}, {1,1,0}, {-1,1,0}, {1,-1,0},
      {0,0,0},{0,0,0},{0,0,0},{0,0,0} },
    { {0,0,0},
      {1,1,1},{1,1,-1},{1,-1,1},{1,-1,-1},
      {-1,1,1},{-1,1,-1},{-1,-1,1},{-1,-1,-1} }
  }; 
  pixel = gv2box(gv);
  pcenter = p = vec_to_vector3(gv.center());
  double d1, d2, d3;
  d1 = (pixel.high.x - pixel.low.x) * 0.5;
  d2 = (pixel.high.y - pixel.low.y) * 0.5;
  d3 = (pixel.high.z - pixel.low.z) * 0.5;
  for (int i = 0; i < num_neighbors[dimensions - 1]; ++i) {
    const geometric_object *o;
    material_type mat;
    vector3 q, shiftby;
    int id;
    q.x = p.x + neighbors[dimensions - 1][i][0] * d1;
    q.y = p.y + neighbors[dimensions - 1][i][1] * d2;
    q.z = p.z + neighbors[dimensions - 1][i][2] * d3;
    o = object_of_point_in_tree(q, geometry_tree, &shiftby, &id);
    if ((id == id1 && vector3_equal(shiftby, shiftby1)) ||
	(id == id2 && vector3_equal(shiftby, shiftby2)))
      continue;
    mat = (o && o->material.which_subclass != MTS::MATERIAL_TYPE_SELF)
      ? o->material : default_material;
    if (id1 == -1) {
      o1 = o;
      shiftby1 = shiftby;
      id1 = id;
      mat1 = mat;
    }
    else if (id2 == -1 || ((id >= id1 && id >= id2) &&
			   (id1 == id2 
			    || material_type_equal(&mat1,&mat2)))) {
      o2 = o;
      shiftby2 = shiftby;
      id2 = id;
      mat2 = mat;
    }
    else if (!(id1 < id2 && 
	       (id1 == id || material_type_equal(&mat1,&mat))) &&
	     !(id2 < id1 &&
	       (id2 == id || material_type_equal(&mat2,&mat))))
      return false;
  }

  // CHECK(id1 > -1, "bug in object_of_point_in_tree?");
  if (id2 == -1) { /* only one nearby object/material */
    id2 = id1;
    o2 = o1;
    mat2 = mat1;
    shiftby2 = shiftby1;
  }

  if ((o1 && variable_material(o1->material.which_subclass)) ||
      (o2 && variable_material(o2->material.which_subclass)) ||
      (variable_material(default_material.which_subclass)
       && (!o1 || !o2 ||
	   o1->material.which_subclass == MTS::MATERIAL_TYPE_SELF ||
	   o2->material.which_subclass == MTS::MATERIAL_TYPE_SELF)))
    return false;

  if (id1 >= id2) {
    *o_front = o1;
    shiftby_front = shiftby1;
    mat_front = mat1;
    if (id1 == id2) mat_behind = mat1; else mat_behind = mat2;
  }
  if (id2 > id1) {
    *o_front = o2;
    shiftby_front = shiftby2;
    mat_front = mat2;
    mat_behind = mat1;
  }
  return true;
}

meep::vec geom_epsilon::normal_vector(const meep::geometric_volume &gv) {
  const geometric_object *o;
  material_type mat, mat_behind;
  vector3 p, shiftby, normal;

  if (!get_front_object(gv, geometry_tree,
			p, &o, shiftby, mat, mat_behind))
    return material_function::normal_vector(gv); // fallback to default

  /* check for trivial case of only one object/material */
  if (material_type_equal(&mat, &mat_behind))
    return meep::zero_vec(gv.dim);

  normal = normal_to_fixed_object(vector3_minus(p, shiftby), *o);
  return vector3_to_vec(unit_vector3(normal));
}

void geom_epsilon::meaneps(double &meps, double &minveps, 
			   meep::vec &n,
			   const meep::geometric_volume &gv,
			   double tol, int maxeval) {
  const geometric_object *o;
  material_type mat, mat_behind;
  vector3 p, shiftby, normal;

  if (!get_front_object(gv, geometry_tree,
			p, &o, shiftby, mat, mat_behind)) {
    fallback_meaneps(meps, minveps, gv, tol, maxeval);
    n = material_function::normal_vector(gv);
    return;
  }

  material_eps(mat, meps, minveps);

  /* check for trivial case of only one object/material */
  if (material_type_equal(&mat, &mat_behind)) { 
    n = meep::zero_vec(gv.dim);
    return;
  }
  
  normal = normal_to_fixed_object(vector3_minus(p, shiftby), *o);
  n = vector3_to_vec(unit_vector3(normal));

  geom_box pixel = gv2box(gv);
  pixel.low = vector3_minus(pixel.low, shiftby);
  pixel.high = vector3_minus(pixel.high, shiftby);

  // fixme: don't ignore maxeval?
  double fill = 1.0 - box_overlap_with_object(pixel, *o, tol, maxeval);
  
  double epsb, epsinvb;
  material_eps(mat_behind, epsb, epsinvb);
  meps += fill * (epsb - meps);
  minveps += fill * (epsinvb - minveps);
}

#ifdef CTL_HAS_COMPLEX_INTEGRATION
static cnumber ceps_func(int n, number *x, void *geomeps_)
{
  geom_epsilon *geomeps = (geom_epsilon *) geomeps_;
  vector3 p = {0,0,0};
  p.x = x[0]; p.y = n > 1 ? x[1] : 0; p.z = n > 2 ? x[2] : 0;
  double s = 1;
  if (dim == meep::Dcyl) { double py = p.y; p.y = p.z; p.z = py; s = p.x; }
  cnumber ret;
  double ep = geomeps->eps(vector3_to_vec(p));
  ret.re = ep * s;
  ret.im = s / ep;
  return ret;
}
#else
static number eps_func(int n, number *x, void *geomeps_)
{
  geom_epsilon *geomeps = (geom_epsilon *) geomeps_;
  vector3 p = {0,0,0};
  double s = 1;
  p.x = x[0]; p.y = n > 1 ? x[1] : 0; p.z = n > 2 ? x[2] : 0;
  if (dim == meep::Dcyl) { double py = p.y; p.y = p.z; p.z = py; s = p.x; }
  return geomeps->eps(vector3_to_vec(p)) * s;
}
static number inveps_func(int n, number *x, void *geomeps_)
{
  geom_epsilon *geomeps = (geom_epsilon *) geomeps_;
  vector3 p = {0,0,0};
  double s = 1;
  p.x = x[0]; p.y = n > 1 ? x[1] : 0; p.z = n > 2 ? x[2] : 0;
  if (dim == meep::Dcyl) { double py = p.y; p.y = p.z; p.z = py; s = p.x; }
  return s / geomeps->eps(vector3_to_vec(p));
}
#endif

// fallback meaneps using libctl's adaptive cubature routine
void geom_epsilon::fallback_meaneps(double &meps, double &minveps,
				    const meep::geometric_volume &gv,
				    double tol, int maxeval)
{
  number esterr;
  integer errflag, n;
  number xmin[3], xmax[3];
  vector3 gvmin, gvmax;
  gvmin = vec_to_vector3(gv.get_min_corner());
  gvmax = vec_to_vector3(gv.get_max_corner());
  xmin[0] = gvmin.x; xmax[0] = gvmax.x; 
  if (dim == meep::Dcyl) {
    xmin[1] = gvmin.z; xmin[2] = gvmin.y; xmax[1] = gvmax.z; xmax[2] = gvmax.y;
  }
  else{
    xmin[1] = gvmin.y; xmin[2] = gvmin.z; xmax[1] = gvmax.y; xmax[2] = gvmax.z;
  }
  if (xmin[2] == xmax[2])
    n = xmin[1] == xmax[1] ? 1 : 2;
  else
    n = 3;
  double vol = 1;
  for (int i = 0; i < n; ++i) vol *= xmax[i] - xmin[i];
  if (dim == meep::Dcyl) vol *= (xmin[0] + xmax[0]) * 0.5;
#ifdef CTL_HAS_COMPLEX_INTEGRATION
  cnumber ret = cadaptive_integration(ceps_func, xmin, xmax, n, (void*) this,
				      0, tol, maxeval, &esterr, &errflag);
  meps = ret.re / vol;
  minveps = ret.im / vol;
#else
  meps = adaptive_integration(eps_func, xmin, xmax, n, (void*) this,
			      0, tol, maxeval, &esterr, &errflag) / vol;
  minveps = adaptive_integration(inveps_func, xmin, xmax, n, (void*) this,
				 0, tol, maxeval, &esterr, &errflag) / vol;
#endif
}

bool geom_epsilon::has_chi3()
{
  for (int i = 0; i < geometry.num_items; ++i) {
    if (geometry.items[i].material.which_subclass == MTS::DIELECTRIC) {
      if (geometry.items[i].material.subclass.dielectric_data->chi3 != 0)
	return true; 
    }
  }
    /* FIXME: what to do about material-functions?
       Currently, we require that at least *one* ordinary material
       property have non-zero chi3 for Kerr to be enabled.   It might
       be better to have set_chi3 automatically delete chi3[] if the
       chi3's are all zero. */
  return (default_material.which_subclass == MTS::DIELECTRIC &&
	  default_material.subclass.dielectric_data->chi3 != 0);
}

double geom_epsilon::chi3(const meep::vec &r) {
  vector3 p = vec_to_vector3(r);

  boolean inobject;
  material_type material =
    material_of_unshifted_point_in_tree_inobject(p, restricted_tree, &inobject);
  
  int destroy_material = 0;
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material = default_material;
  }
  if (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material = eval_material_func(material.subclass.
				  material_function_data->material_func,
				  p);
    destroy_material = 1;
  }
  
  double chi3_val;
  switch (material.which_subclass) {
  case MTS::DIELECTRIC:
    chi3_val = material.subclass.dielectric_data->chi3;
    break;
  default:
    chi3_val = 0;
  }
  
  if (destroy_material)
    material_type_destroy(material);
  
  return chi3_val;
}

bool geom_epsilon::has_chi2()
{
  for (int i = 0; i < geometry.num_items; ++i) {
    if (geometry.items[i].material.which_subclass == MTS::DIELECTRIC) {
      if (geometry.items[i].material.subclass.dielectric_data->chi2 != 0)
	return true; 
    }
  }
    /* FIXME: what to do about material-functions?
       Currently, we require that at least *one* ordinary material
       property have non-zero chi2 for Kerr to be enabled.   It might
       be better to have set_chi2 automatically delete chi2[] if the
       chi2's are all zero. */
  return (default_material.which_subclass == MTS::DIELECTRIC &&
	  default_material.subclass.dielectric_data->chi2 != 0);
}

double geom_epsilon::chi2(const meep::vec &r) {
  vector3 p = vec_to_vector3(r);

  boolean inobject;
  material_type material =
    material_of_unshifted_point_in_tree_inobject(p, restricted_tree, &inobject);
  
  int destroy_material = 0;
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material = default_material;
  }
  if (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material = eval_material_func(material.subclass.
				  material_function_data->material_func,
				  p);
    destroy_material = 1;
  }
  
  double chi2_val;
  switch (material.which_subclass) {
  case MTS::DIELECTRIC:
    chi2_val = material.subclass.dielectric_data->chi2;
    break;
  default:
    chi2_val = 0;
  }
  
  if (destroy_material)
    material_type_destroy(material);
  
  return chi2_val;
}

double geom_epsilon::sigma(const meep::vec &r) {
  vector3 p = vec_to_vector3(r);

  boolean inobject;
  material_type material =
    material_of_unshifted_point_in_tree_inobject(p, restricted_tree, &inobject);
  
  int destroy_material = 0;
  if (material.which_subclass == MTS::MATERIAL_TYPE_SELF) {
    material = default_material;
  }
  if (material.which_subclass == MTS::MATERIAL_FUNCTION) {
    material = eval_material_func(material.subclass.
				  material_function_data->material_func,
				  p);
    destroy_material = 1;
  }
  
  double sigma = 0;
  if (material.which_subclass == MTS::DIELECTRIC) {
    polarizability_list plist = 
      material.subclass.dielectric_data->polarizations;
    for (int j = 0; j < plist.num_items; ++j)
      if (plist.items[j].omega == omega &&
	  plist.items[j].gamma == gamma &&
	  plist.items[j].delta_epsilon == deps &&
	  plist.items[j].energy_saturation == energy_sat) {
	sigma = plist.items[j].sigma;
	break;
      }
  }
  
  if (destroy_material)
    material_type_destroy(material);

  return sigma;
}

struct pol {
  double omega, gamma, deps, esat;
  struct pol *next;
};

// add a polarization to the list if it is not already there
static pol *add_pol(pol *pols,
		    double omega, double gamma, double deps, double esat)
{
  struct pol *p = pols;
  while (p && !(p->omega == omega && p->gamma == gamma
		&& p->deps == deps && p->esat == esat))
    p = p->next;
  if (!p) {
    p = new pol;
    p->omega = omega;
    p->gamma = gamma;
    p->deps = deps;
    p->esat = esat;
    p->next = pols;
    pols = p;
  }
  return pols;
}

static pol *add_pols(pol *pols, const polarizability_list plist) {
  for (int j = 0; j < plist.num_items; ++j) {
    pols = add_pol(pols,
		   plist.items[j].omega, plist.items[j].gamma,
		   plist.items[j].delta_epsilon,
		   plist.items[j].energy_saturation);
  }
  return pols;
}

void geom_epsilon::add_polarizabilities(meep::structure *s) {
  pol *pols = 0;

  // construct a list of the unique polarizabilities in the geometry:
  for (int i = 0; i < geometry.num_items; ++i) {
    if (geometry.items[i].material.which_subclass == MTS::DIELECTRIC)
      pols = add_pols(pols, geometry.items[i].material
		      .subclass.dielectric_data->polarizations);
  }
  if (default_material.which_subclass == MTS::DIELECTRIC)
    pols = add_pols(pols, default_material
		    .subclass.dielectric_data->polarizations);
    
  for (struct pol *p = pols; p; p = p->next) {
    master_printf("polarizability: omega=%g, gamma=%g, deps=%g, esat=%g\n",
		  p->omega, p->gamma, p->deps, p->esat);
    s->add_polarizability(*this, p->omega, p->gamma, p->deps, p->esat);
  }
  
  while (pols) {
    struct pol *p = pols;
    pols = pols->next;
    delete p;
  }
}

/***********************************************************************/

meep::structure *make_structure(int dims, vector3 size, vector3 center,
				double resolution, bool enable_averaging,
				double subpixel_tol, int subpixel_maxeval,
				bool ensure_periodicity_p,
				geometric_object_list geometry,
				material_type default_mat,
				pml_list pml_layers,
				symmetry_list symmetries,
				int num_chunks, double Courant)
{
  master_printf("-----------\nInitializing structure...\n");
  
  // only cartesian lattices are currently allowed
  geom_initialize();
  geometry_center = center;
  
  number no_size = 2.0 / ctl_get_number("infinity");
  if (size.x <= no_size)
    size.x = 0.0;
  if (size.y <= no_size)
    size.y = 0.0;
  if (size.z <= no_size)
    size.z = 0.0;
  
  set_dimensions(dims);
  
  geometry_lattice.size = size;

  master_printf("Working in %s dimensions.\n", meep::dimension_name(dim));
  
  meep::volume v;
  switch (dims) {
  case 0: case 1:
    v = meep::vol1d(size.z, resolution);
    break;
  case 2:
    v = meep::vol2d(size.x, size.y, resolution);
    break;
  case 3:
    v = meep::vol3d(size.x, size.y, size.z, resolution);
    break;
  case CYLINDRICAL:
    v = meep::volcyl(size.x, size.z, resolution);
    break;
  default:
    CK(0, "unsupported dimensionality");
  }
  v.center_origin();
  v.shift_origin(vector3_to_vec(center));
  
  meep::symmetry S;
  for (int i = 0; i < symmetries.num_items; ++i) 
    switch (symmetries.items[i].which_subclass) {
    case symmetry::SYMMETRY_SELF: break; // identity
    case symmetry::MIRROR_SYM:
      S = S + meep::mirror(meep::direction(symmetries.items[i].direction), v)
	* complex<double>(symmetries.items[i].phase.re,
			  symmetries.items[i].phase.im);
      break;
    case symmetry::ROTATE2_SYM:
      S = S + meep::rotate2(meep::direction(symmetries.items[i].direction), v)
	* complex<double>(symmetries.items[i].phase.re,
			  symmetries.items[i].phase.im);
      break;
    case symmetry::ROTATE4_SYM:
      S = S + meep::rotate4(meep::direction(symmetries.items[i].direction), v)
	* complex<double>(symmetries.items[i].phase.re,
			  symmetries.items[i].phase.im);
      break;
    }

  meep::boundary_region br;
  for (int i = 0; i < pml_layers.num_items; ++i) {
    using namespace meep;
    if (pml_layers.items[i].direction == -1) {
      LOOP_OVER_DIRECTIONS(v.dim, d) {
	if (pml_layers.items[i].side == -1) {
	  FOR_SIDES(b)
	    br = br + meep::boundary_region(meep::boundary_region::PML,
					    pml_layers.items[i].thickness,
					    pml_layers.items[i].strength,
					    d, b);
	}
	else
	  br = br + meep::boundary_region(meep::boundary_region::PML,
					  pml_layers.items[i].thickness,
					  pml_layers.items[i].strength,
					  d,
					  (meep::boundary_side) 
					  pml_layers.items[i].side);
      }
    }
    else {
	if (pml_layers.items[i].side == -1) {
	  FOR_SIDES(b)
	    br = br + meep::boundary_region(meep::boundary_region::PML,
					    pml_layers.items[i].thickness,
					    pml_layers.items[i].strength,
					    (meep::direction)
					    pml_layers.items[i].direction,
					    b);
	}
	else
	  br = br + meep::boundary_region(meep::boundary_region::PML,
					  pml_layers.items[i].thickness,
					  pml_layers.items[i].strength,
					  (meep::direction)
					  pml_layers.items[i].direction,
					  (meep::boundary_side) 
					  pml_layers.items[i].side);
    }
  }
  
  ensure_periodicity = ensure_periodicity_p;
  default_material = default_mat;
  geom_epsilon geps(geometry, v.pad().surroundings());

  if (subpixel_maxeval < 0) subpixel_maxeval = 0; // no limit

  meep::structure *s = new meep::structure(v, geps, br, S, 
					   num_chunks, Courant,
					   enable_averaging,
					   subpixel_tol,
					   subpixel_maxeval);

  geps.add_polarizabilities(s);

  master_printf("-----------\n");
  
  return s;
}

/*************************************************************************/
