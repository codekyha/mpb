/* Copyright (C) 1999 Massachusetts Institute of Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**************************************************************************/

/* Here, we define the external functions callable from Guile, as defined
   by photon.scm. */

/**************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

/* GNU Guile library header file: */
#include <guile/gh.h>

/* Header files for my eigensolver routines: */
#include <config.h>
#include <check.h>
#include <blasglue.h>
#include <matrices.h>
#include <eigensolver.h>
#include <maxwell.h>

/* Header file for the ctl-file (Guile) interface; automatically
   generated from photon.scm */
#include <ctl-io.h>

/* Routines from libctl/utils/libgeom: */
#include <geom.h>

/**************************************************************************/

/* a couple of utilities to convert libctl data types to the data
   types of the eigensolver & maxwell routines: */

static void vector3_to_arr(real arr[3], vector3 v)
{
     arr[0] = v.x;
     arr[1] = v.y;
     arr[2] = v.z;
}

static void matrix3x3_to_arr(real arr[3][3], matrix3x3 m)
{
     vector3_to_arr(arr[0], m.c0);
     vector3_to_arr(arr[1], m.c1);
     vector3_to_arr(arr[2], m.c2);
}

/**************************************************************************/

#define NWORK 3
#define NUM_FFT_BANDS 20 /* max number of bands to FFT at a time */

/* global variables for retaining data about the eigenvectors between
   calls from Guile: */

static maxwell_data *mdata = NULL;
static maxwell_target_data *mtdata = NULL;
static evectmatrix H, W[NWORK];

static scalar_complex *curfield = NULL;
static int curfield_band;
static char curfield_type;

/* R[i]/G[i] are lattice/reciprocal-lattice vectors */
static real R[3][3], G[3][3];
static matrix3x3 Rm, Gm; /* same thing, but matrix3x3 */

/* index of current kpoint, for labeling output */
static int kpoint_index = 0;

/**************************************************************************/

/* Given a position r in the basis of the lattice vectors, return the
   corresponding dielectric constant.  edata is ignored.  Should be
   called from within init_params (or after init_params), so that the
   geometry input variables will have been read in (for use in libgeom).

   This function is passed to set_maxwell_dielectric to initialize
   the dielectric tensor array for eigenvector calculations. */

static real epsilon_func(real r[3], void *edata)
{
     material_type material;
     vector3 p;

     /* p needs to be in the lattice *unit* vector basis, while r is
	in the lattice vector basis.  Also, shift origin to the center
        of the grid. */
     p.x = r[0] * geometry_lattice.size.x - 0.5;
     p.y = r[1] * geometry_lattice.size.y - 0.5;
     p.z = r[2] * geometry_lattice.size.z - 0.5;
     
     material = material_of_point(p);  /* from libctl/utils/libgeom/geom.c */
     return material.epsilon;
}

/**************************************************************************/

/* Set the current polarization to solve for. (init-params should have
   already been called.  (Guile-callable; see photon.scm.) 

   Also initializes the field to random numbers.

   Hackery: If a polarization constant -(p+1) is substituted for p,
   then the field is not reinitialized unless the polarization has
   changed since the last call.  

   p = 0 means NO_POLARIZATION
   p = 1 means TE_POLARIZATION
   p = 2 means TM_POLARIZATION
   p = 3 means the polarization of the previous call, 
       or NO_POLARIZATION if this is the first call */

void set_polarization(int p)
{
     static int last_p = 0xDEADBEEF;  /* initialize to some non-value */
     int real_p, i;

     if (!mdata) {
	  fprintf(stderr,
		  "init-params must be called before set-polarization!\n");
	  return;
     }
     
     if (p < 0)
	  real_p = -(p + 1);
     else
	  real_p = p;

     if (real_p == 3)
	  real_p = last_p == 0xDEADBEEF ? 0 : last_p;

     switch (real_p) {
	 case 0:
	      printf("Solving for non-polarized bands.\n");
	      set_maxwell_data_polarization(mdata, NO_POLARIZATION);
	      break;
	 case 1:
	      printf("Solving for TE-polarized bands.\n");
	      set_maxwell_data_polarization(mdata, TE_POLARIZATION);
	      break;
	 case 2:
	      printf("Solving for TM-polarized bands.\n");
	      set_maxwell_data_polarization(mdata, TM_POLARIZATION);
	      break;
	 default:
	      fprintf(stderr, "Unknown polarization type!\n");
	      return;
     }

     if (p > 0 || real_p != last_p) {
	  /* need to re-randomize fields */
	  printf("Initializing fields to random numbers...\n");
	  for (i = 0; i < H.n * H.p; ++i)
	       ASSIGN_REAL(H.data[i], rand() * 1.0 / RAND_MAX);
     }
     last_p = real_p;
     kpoint_index = 0;  /* reset index */
}

/**************************************************************************/

/* Guile-callable function: init-params, which initializes any data
   that we need for the eigenvalue calculation.  When this function
   is called, the input variables (the geometry, etcetera) have already
   been read into the global variables defined in ctl-io.h.  

   If reset_fields is false, then any fields from a previous run are
   retained if they are of the same dimensions.  Otherwise, new
   fields are allocated and initialized to random numbers. */
void init_params(boolean reset_fields)
{
     int i, local_N, N_start, alloc_N;
     int nx, ny, nz;
     int mesh[3];
     int have_old_fields = 0;
     
     /* Output a bunch of stuff so that the user can see what we're
	doing and what we've read in. */
     
     printf("init-params: initializing eigensolver data\n");
     
     printf("Computing %d bands with %e tolerance.\n", num_bands, tolerance);
     if (target_freq != 0.0)
	  printf("Target frequency is %g\n", target_freq);
     
     nx = grid_size.x;
     ny = grid_size.y;
     nz = grid_size.z;

     {
	  int true_rank = nz > 1 ? 3 : (ny > 1 ? 2 : 1);
	  if (true_rank < dimensions)
	       dimensions = true_rank;
	  else if (true_rank > dimensions)
	       fprintf(stderr, "WARNING: true rank of grid is > dimensions\n");
     }

     printf("Working in %d dimensions.\n", dimensions);

     printf("Grid size is %d x %d x %d.\n", nx, ny, nz);

     printf("Mesh size is %d.\n", mesh_size);
     mesh[0] = mesh_size;
     mesh[1] = (dimensions > 1) ? mesh_size : 1;
     mesh[2] = (dimensions > 2) ? mesh_size : 1;

     Rm.c0 = vector3_scale(geometry_lattice.size.x, geometry_lattice.basis.c0);
     Rm.c1 = vector3_scale(geometry_lattice.size.y, geometry_lattice.basis.c1);
     Rm.c2 = vector3_scale(geometry_lattice.size.z, geometry_lattice.basis.c2);
     printf("Lattice vectors:\n");
     printf("     (%g, %g, %g)\n", Rm.c0.x, Rm.c0.y, Rm.c0.z);  
     printf("     (%g, %g, %g)\n", Rm.c1.x, Rm.c1.y, Rm.c1.z);
     printf("     (%g, %g, %g)\n", Rm.c2.x, Rm.c2.y, Rm.c2.z);
  
     Gm = matrix3x3_inverse(Rm);
     printf("Reciprocal lattice vectors:\n");
     printf("     (%g, %g, %g)\n", Gm.c0.x, Gm.c0.y, Gm.c0.z);  
     printf("     (%g, %g, %g)\n", Gm.c1.x, Gm.c1.y, Gm.c1.z);
     printf("     (%g, %g, %g)\n", Gm.c2.x, Gm.c2.y, Gm.c2.z);
     
     matrix3x3_to_arr(R, Rm);
     matrix3x3_to_arr(G, Gm);

     printf("Geometric objects:\n");
     for (i = 0; i < geometry.num_items; ++i) {
	  display_geometric_object_info(5, geometry.items[i]);
	  printf("%*sdielectric constant epsilon = %g\n", 5 + 5, "",
		 geometry.items[i].material.epsilon);
     }

     printf("%d k-points:\n", k_points.num_items);
     for (i = 0; i < k_points.num_items; ++i)
	  printf("     (%g,%g,%g)\n", k_points.items[i].x,
		 k_points.items[i].y, k_points.items[i].z);
     
     if (mdata) {  /* need to clean up from previous init_params call */
	  if (nx == mdata->nx && ny == mdata->ny && nz == mdata->nz &&
	      num_bands == mdata->num_bands)
	       have_old_fields = 1; /* don't need to reallocate */
	  else {
	       destroy_evectmatrix(H);
	       for (i = 0; i < NWORK; ++i)
		    destroy_evectmatrix(W[i]);
	  }
	  destroy_maxwell_target_data(mtdata); mtdata = NULL;
	  destroy_maxwell_data(mdata); mdata = NULL;
	  curfield = NULL;
     }
     else
	  srand(time(NULL)); /* init random seed for field initialization */
   
     printf("Creating Maxwell data...\n");
     mdata = create_maxwell_data(nx, ny, nz, &local_N, &N_start, &alloc_N,
                                 num_bands, NUM_FFT_BANDS);
     CHECK(mdata, "NULL mdata");

     printf("Initializing dielectric function...\n");
     set_maxwell_dielectric(mdata, mesh, R, epsilon_func, NULL);

     if (target_freq != 0.0)
	  mtdata = create_maxwell_target_data(mdata, target_freq);
     else
	  mtdata = NULL;

     if (!have_old_fields) {
	  printf("Allocating fields...\n");
	  H = create_evectmatrix(nx * ny * nz, 2, num_bands,
				 local_N, N_start, alloc_N);
	  for (i = 0; i < NWORK; ++i)
	       W[i] = create_evectmatrix(nx * ny * nz, 2, num_bands,
					 local_N, N_start, alloc_N);
     }

     if (!have_old_fields || reset_fields)
	  set_polarization(0);
     else
	  set_polarization(-(3+1)); /* make same as prev. call, but
				       don't reinitialize the fields. */

     printf("Stuff for grepping:\n");
     printf("sumfrq:, k index, kx, ky, kz, kmag/2pi");
     for (i = 0; i < num_bands; ++i)
	  printf(", band %d", i + 1);
     printf("\n");
     if (dimensions <= 2) {
	  printf("sumte:, k index, kx, ky, kz, kmag/2pi");
	  for (i = 0; i < num_bands; ++i)
	       printf(", band %d", i + 1);
	  printf("\n");
	  printf("sumtm:, k index, kx, ky, kz, kmag/2pi");
	  for (i = 0; i < num_bands; ++i)
	       printf(", band %d", i + 1);
	  printf("\n");
     }
}

/**************************************************************************/

/* Solve for the bands at a given k point.
   Must only be called after init_params! */
void solve_kpoint(vector3 kvector)
{
     int i, num_iters;
     real *eigvals;
     real k[3];

     printf("solve_kpoint (%g,%g,%g):\n",
	    kvector.x, kvector.y, kvector.z);

     if (!mdata) {
	  fprintf(stderr, "init-params must be called before solve-kpoint!\n");
	  return;
     }

     vector3_to_arr(k, kvector);
     update_maxwell_data_k(mdata, k, G[0], G[1], G[2]);

     eigvals = (real*) malloc(sizeof(real) * num_bands);
     CHECK(eigvals, "out of memory");

     printf("Solving for bands...\n");

     if (mtdata) {  /* solving for bands near a target frequency */
	  eigensolver(H, eigvals,
		      maxwell_target_operator, (void *) mtdata,
		      maxwell_target_preconditioner, (void *) mtdata, NULL,
		      maxwell_constraint, (void *) mdata,
		      W, NWORK, tolerance, &num_iters);
	  /* now, diagonalize the real Maxwell operator in the
	     solution subspace to get the true eigenvalues and
	     eigenvectors: */
	  eigensolver_get_eigenvals(H, eigvals, maxwell_operator, mdata,
                                    W[0], W[1]);
     }
     else
	  eigensolver(H, eigvals,
		      maxwell_operator, (void *) mdata,
		      maxwell_preconditioner, (void *) mdata, NULL,
		      maxwell_constraint, (void *) mdata,
		      W, NWORK, tolerance, &num_iters);

     printf("Finished solving for bands after %d iterations.\n", num_iters);
     
     if (num_write_output_vars > 1)
	  destroy_output_vars(); /* we are required by libctl to call this
				    in order to deallocate output vars from
				    previous calls. */

     /* create freqs array for storing frequencies in a Guile list */
     freqs.num_items = num_bands;
     freqs.items = (number *) malloc(freqs.num_items * sizeof(number));
     CHECK(freqs.items, "out of memory");
     
     printf("%s:, %d, %g, %g, %g, %g",
	    mdata->polarization == NO_POLARIZATION ? "sumfrq" :
	    (mdata->polarization == TE_POLARIZATION ? "sumte" : "sumtm"),
	    ++kpoint_index, k[0], k[1], k[2],
	    vector3_norm(matrix3x3_vector3_mult(Gm, kvector)));
     for (i = 0; i < num_bands; ++i) {
	  freqs.items[i] = sqrt(eigvals[i]);
	  printf(", %g", freqs.items[i]);
     }
     printf("\n");

     free(eigvals);
     curfield = NULL;
}

/**************************************************************************/

/* The following routines take the eigenvectors computed by solve-kpoint
   and compute the field (D, H, or E) in position space for one of the bands.
   This field is stored in the global curfield (actually an alias for
   mdata->fft_data, since the latter is unused and big enough).  This
   field can then be manipulated with subsequent "*-field-*" functions
   below.  You can also get the scalar field, epsilon.

   All of these functions are designed to be called by the user
   via Guile. */

void get_dfield(int which_band)
{
     if (!mdata) {
	  fprintf(stderr, "init-params must be called before get-dfield!\n");
	  return;
     }
     if (!kpoint_index) {
	  fprintf(stderr, "solve-kpoint must be called before get-dfield!\n");
	  return;
     }
     if (which_band >= mdata->num_bands) {
	  fprintf(stderr, "band index must be < num_bands (%d)\n",
		  mdata->num_bands);
     }

     curfield = (scalar_complex *) mdata->fft_data;
     curfield_band = which_band;
     curfield_type = 'd';
     maxwell_compute_dfield(mdata, H, curfield, which_band, 1);
}

void get_hfield(int which_band)
{
     if (!mdata) {
	  fprintf(stderr, "init-params must be called before get-hfield!\n");
	  return;
     }
     if (!kpoint_index) {
	  fprintf(stderr, "solve-kpoint must be called before get-hfield!\n");
	  return;
     }
     if (which_band >= mdata->num_bands) {
	  fprintf(stderr, "band index must be < num_bands (%d)\n",
		  mdata->num_bands);
     }

     curfield = (scalar_complex *) mdata->fft_data;
     curfield_band = which_band;
     curfield_type = 'h';
     maxwell_compute_hfield(mdata, H, curfield, which_band, 1);
}

void get_efield_from_dfield(void)
{
     if (!curfield || curfield_type != 'd') {
	  fprintf(stderr, "get-dfield must be called before "
		  "get-efield-from-dfield!\n");
	  return;
     }
     CHECK(mdata, "unexpected NULL mdata");
     maxwell_compute_e_from_d(mdata, curfield, 1);
     curfield_type = 'e';
}

void get_efield(int which_band)
{
     get_dfield(which_band);
     get_efield_from_dfield();
}

/* get the dielectric function, and compute some statistics */
void get_epsilon(void)
{
     int i, N;
     real *epsilon;
     real eps_mean = 0, eps_inv_mean = 0, eps_high = -1e20, eps_low = 1e20;
     int fill_count = 0;

     if (!mdata) {
	  fprintf(stderr, "init-params must be called before get-epsilon!\n");
	  return;
     }

     curfield = (scalar_complex *) mdata->fft_data;
     epsilon = (real *) curfield;
     curfield_band = -1;
     curfield_type = 'n';

     /* get epsilon.  Recall that we actually have an inverse
	dielectric tensor at each point; define an average index by
	the inverse of the average eigenvalue of the 1/eps tensor.
	i.e. 3/(trace 1/eps). */

     N = mdata->fft_output_size;
     for (i = 0; i < N; ++i) {
	  epsilon[i] = 3.0 / (mdata->eps_inv[i].m00 +
			      mdata->eps_inv[i].m11 +
			      mdata->eps_inv[i].m22);
	  if (epsilon[i] < eps_low)
	       eps_low = epsilon[i];
	  if (epsilon[i] > eps_high)
	       eps_high = epsilon[i];
	  eps_mean += epsilon[i];
	  eps_inv_mean += 1/epsilon[i];
	  if (epsilon[i] > 1.0001)
	       ++fill_count;
     }
     printf("eps goes from %g-%g, mean %g, harmonic mean %g, "
	    "%g%% fill\n", eps_low, eps_high, eps_mean/N, N/eps_inv_mean,
	    (100.0 * fill_count) / N);
}

/**************************************************************************/

/* Replace curfield (either d or h) with the scalar energy density function,
   normalized to one.  While we're at it, compute some statistics about
   the relative strength of different field components. */
void compute_field_energy(void)
{
     int i, N;
     real comp_sum[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
     real energy_sum = 0.0, normalization;
     real *energy_density = (real *) curfield;

     if (!curfield || !strchr("dh", curfield_type)) {
	  fprintf(stderr, "The D or H field must be loaded first.\n");
	  return;
     }

     N = mdata->fft_output_size;
     for (i = 0; i < N; ++i) {
	  scalar_complex field[3];
	  real
	       comp_sqr0,comp_sqr1,comp_sqr2,comp_sqr3,comp_sqr4,comp_sqr5;

	  /* energy is either |curfield|^2 or |curfield|^2 / epsilon,
	     depending upon whether it is H or D. */
	  if (curfield_type == 'h') {
	       field[0] =   curfield[3*i];
	       field[1] = curfield[3*i+1];
	       field[2] = curfield[3*i+2];
	  }
	  else
	       assign_symmatrix_vector(field, mdata->eps_inv[i], curfield+3*i);

	  comp_sum[0] += comp_sqr0 = field[0].re *   curfield[3*i].re;
	  comp_sum[1] += comp_sqr1 = field[0].im *   curfield[3*i].im;
	  comp_sum[2] += comp_sqr2 = field[1].re * curfield[3*i+1].re;
	  comp_sum[3] += comp_sqr3 = field[1].im * curfield[3*i+1].im;
	  comp_sum[4] += comp_sqr4 = field[2].re * curfield[3*i+2].re;
	  comp_sum[5] += comp_sqr5 = field[2].im * curfield[3*i+2].im;

	  /* Note: here, we write to energy_density[i]; this is
	     safe, even though energy_density is aliased to curfield,
	     since energy_density[i] is guaranteed to come at or before
	     curfield[i] (which we are now done with). */

	  energy_sum += energy_density[i] = 
	       comp_sqr0+comp_sqr1+comp_sqr2+comp_sqr3+comp_sqr4+comp_sqr5;
     }

     normalization = 1.0 / energy_sum;
     for (i = 0; i < N; ++i)
	  energy_density[i] *= normalization;

     printf("%c-energy-components:, %d, %d",
	    curfield_type, kpoint_index, curfield_band);
     for (i = 0; i < 6; ++i) {
	  comp_sum[i] *= normalization;
	  if (i % 2 == 1)
	       printf(", %g", comp_sum[i] + comp_sum[i-1]);
     }
     printf("\n");

     /* remember that we now have energy density; denoted by capital D/H */
     curfield_type = toupper(curfield_type);
}

/**************************************************************************/

/* compute the fraction of the field energy that is located in the
   given range of dielectric constants: */
number compute_energy_in_dielectric(number eps_low, number eps_high)
{
     int N, i;
     real *energy = (real *) curfield;
     real epsilon, energy_sum = 0.0;

     if (!curfield || !strchr("DH", curfield_type)) {
          fprintf(stderr, "The D or H energy density must be loaded first.\n");
          return;
     }

     N = mdata->fft_output_size;
     for (i = 0; i < N; ++i) {
	  epsilon = 3.0 / (mdata->eps_inv[i].m00 +
			   mdata->eps_inv[i].m11 +
			   mdata->eps_inv[i].m22);
	  if (epsilon >= eps_low && epsilon <= eps_high)
	       energy_sum += energy[i];
     }
     return energy_sum;
}

/**************************************************************************/

/* given the field in curfield, store it to HDF (or whatever) using
   the matrixio (fieldio) routines.  Allow the user to specify that
   the fields be periodically extended, so that several lattice cells
   are stored. */
void output_field_extended(vector3 copiesv)
{
     char fname[100], description[100];
     int dims[3], local_nx, local_x_start;
     int copies[3] = { copiesv.x, copiesv.y, copiesv.z };

     if (!curfield) {
	  fprintf(stderr, 
		  "fields, energy dens., or epsilon must be loaded first.\n");
	  return;
     }
     
     /* this will need to be fixed for MPI, where we transpose the data,
	and also for real-to-complex calculations: */
#if defined(HAVE_MPI) || ! defined(SCALAR_COMPLEX)
#    broken, please fix
#endif
     dims[0] = mdata->nx;
     dims[1] = mdata->ny;
     dims[2] = mdata->nz;
     local_nx = mdata->local_nx;
     local_x_start = mdata->local_x_start;
     
     if (strchr("dhe", curfield_type)) { /* outputting vector field */
	  sprintf(fname, "%c.%02d.%02d",
		  curfield_type, kpoint_index, curfield_band);
	  sprintf(description, "%c field, kpoint %d, band %d, freq=%g",
		  curfield_type, kpoint_index, curfield_band, 
		  freqs.items[curfield_band]);
	  printf("Outputting fields to %s...\n", fname);
	  fieldio_write_complex_field(curfield, 3, dims,
				      local_nx, local_x_start,
				      copies, mdata->current_k, R,
				      fname, description);
     }
     else if (strchr("DHn", curfield_type)) { /* scalar field */
	  if (curfield_type == 'n') {
	       sprintf(fname, "epsilon");
	       sprintf(description, "dielectric function, epsilon");
	  }
	  else {
	       sprintf(fname, "%cpwr.%02d.%02d",
		       tolower(curfield_type), kpoint_index, curfield_band);
	       sprintf(description,
		       "%c field energy density, kpoint %d, band %d, freq=%g",
		       curfield_type, kpoint_index, curfield_band, 
		       freqs.items[curfield_band]);
	  }
	   printf("Outputting %s...\n", fname);
	   fieldio_write_real_vals((real *) curfield, 3, dims,
				   local_nx, local_x_start, copies,
				   fname, description);
     }
     else
	  fprintf(stderr, "unknown field type!\n");
}

/**************************************************************************/

/* For curfield an energy density, compute the fraction of the energy
   that resides inside the given list of geometric objects.   Later
   objects in the list have precedence, just like the ordinary
   geometry list. */
number compute_energy_in_object_list(geometric_object_list objects)
{
     int i, j, k, n1, n2, n3;
     real s1, s2, s3;
     real *energy = (real *) curfield;
     real energy_sum = 0;

     if (!curfield || !strchr("DH", curfield_type)) {
          fprintf(stderr, "The D or H energy density must be loaded first.\n");
          return;
     }

     /* this will need to be fixed for MPI, where we transpose the data,
	and also for real-to-complex calculations: */
#if defined(HAVE_MPI) || ! defined(SCALAR_COMPLEX)
#    broken, please fix
#endif
     n1 = mdata->nx;
     n2 = mdata->ny;
     n3 = mdata->nz;
     s1 = geometry_lattice.size.x / n1;
     s2 = geometry_lattice.size.y / n2;
     s3 = geometry_lattice.size.z / n3;

     for (i = 0; i < n1; ++i)
	  for (j = 0; j < n2; ++j)
	       for (k = 0; k < n3; ++k) {
		    vector3 p = { i * s1, j * s2, k * s3 };
		    int n;
		    for (n = objects.num_items - 1; n >= 0; --n)
			 if (point_in_periodic_objectp(p, objects.items[n])) {
			      energy_sum += energy[(i*n2 + j)*n3 + k];
			      break;
			 }
	       }
     return energy_sum;
}

/**************************************************************************/