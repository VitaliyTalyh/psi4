#ifndef SAPT_H
#define SAPT_H

#include <psiconfig.h>

#ifdef _OPENMP
  #include <omp.h>
#endif

#ifdef HAVE_MKL
  #include <mkl.h>
#endif

#include <libmints/mints.h>
#include <libpsio/psio.h>
#include <libciomr/libciomr.h>
#include <libqt/qt.h>
#include <lib3index/3index.h>
#include <psi4-dec.h>

#define INDEX(i,j) ((i>=j) ? (ioff[i] + j) : (ioff[j] + i))

using namespace psi;

namespace psi { namespace sapt {

class SAPT : public Wavefunction {

private:
  void initialize();

protected:
  shared_ptr<BasisSet> ribasis_;
  shared_ptr<BasisSet> zero_;

  int nso_;
  int nmo_;
  int ndf_;
  int noccA_;  
  int foccA_;  
  int aoccA_;  
  int noccB_;  
  int foccB_;  
  int aoccB_;  
  int nvirA_;
  int nvirB_;
  int NA_;
  int NB_;

  int print_;
  bool debug_;

  long int mem_;

  double enuc_; 
  double eHF_; 
  double schwarz_; 

  double *evalsA_;
  double *evalsB_;
  double *diagAA_;
  double *diagBB_;

  double **CA_;
  double **CB_; 
  double **CHFA_;
  double **CHFB_; 
  double **sAB_;
  double **vABB_;
  double **vBAA_;
  double **vAAB_;
  double **vBAB_;

  void zero_disk(int, const char *, int, int);

public:
  SAPT(Options& options, shared_ptr<PSIO> psio, shared_ptr<Chkpt> chkpt);
  virtual ~SAPT();

  virtual double compute_energy()=0;
};

class CPHFDIIS {

private:
  int max_diis_vecs_;
  int vec_length_;

  int curr_vec_;
  int num_vecs_;

  double **t_vecs_;
  double **err_vecs_;

protected:

public:
  CPHFDIIS(int, int);
  ~CPHFDIIS();

  void store_vectors(double *, double *);
  void get_new_vector(double *);
};

}}

#endif
