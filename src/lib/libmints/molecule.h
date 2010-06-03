#ifndef _psi_src_lib_libmints_molecule_h_
#define _psi_src_lib_libmints_molecule_h_

#include <vector>
#include <string>
#include <cstdio>

#include <libmints/vector3.h>
#include <libmints/vector.h>
#include <libmints/matrix.h>

#include <libpsio/psio.hpp>
#include <libchkpt/chkpt.hpp>

namespace psi {

class PointGroup;

extern FILE *outfile;

/*! \ingroup MINTS
 *  \class Molecule
 *  \brief Molecule information class.
 */
class Molecule
{
public:
    struct atom_info {
        double x, y, z;
        int Z;				// if Z == dummy atom
        double charge;
        double mass;
        std::string label;
    };

protected:
    /// Atom info vector (no knowledge of dummy atoms)
    std::vector<atom_info> atoms_;
    /// Atom info vector (includes dummy atoms)
    std::vector<atom_info> full_atoms_;
    /// Symmetry information about the molecule
    int nirreps_;
    /// Zero it out
    void clear();

    // Point group to use with this molecule.
    boost::shared_ptr<PointGroup> pg_;
    
    /// Number of unique atoms
    int nunique_;
    int *nequiv_;
    int **equiv_;
    int *atom_to_unique_;

public:
    Molecule();
    /// Copy constructor.
    Molecule(const Molecule& other);
    virtual ~Molecule();

    /// Assignment operator.
    Molecule& operator=(const Molecule& other);

    /// Pull information from a chkpt object created from psio
    void init_with_psio(shared_ptr<PSIO> psio);
    /// Pull information from the chkpt object passed
    void init_with_chkpt(shared_ptr<Chkpt> chkpt);
    /// Pull information from an XYZ file
    void init_with_xyz(const std::string& xyzfilename);
    
    /// Add an atom to the molecule
    void add_atom(int Z, double x, double y, double z,
                  const char * = 0, double mass = 0.0,
                  int have_charge = 0, double charge = 0.0);

    /// Number of atoms
    int natom() const { return atoms_.size(); }
    /// Number of all atoms (includes dummies)
    int nallatom() const { return full_atoms_.size(); }
    /// Nuclear charge of atom
    int Z(int atom) const { return atoms_[atom].Z; }
    /// Nuclear charge of atom
    int fZ(int atom) const { return full_atoms_[atom].Z; }
    // x position of atom
    double x(int atom) const { return atoms_[atom].x; }
    // y position of atom
    double y(int atom) const { return atoms_[atom].y; }
    // z position of atom
    double z(int atom) const { return atoms_[atom].z; }
     // x position of atom
    double fx(int atom) const { return full_atoms_[atom].x; }
    // y position of atom
    double fy(int atom) const { return full_atoms_[atom].y; }
    // z position of atom
    double fz(int atom) const { return full_atoms_[atom].z; }
   /// Return reference to atom_info struct for atom
    const atom_info &r(int atom) const { return atoms_[atom]; }
    /// Return copy of atom_info for atom
    atom_info r(int atom) { return atoms_[atom]; }
   /// Return reference to atom_info struct for atom in full atoms
    const atom_info &fr(int atom) const { return full_atoms_[atom]; }
    /// Return copy of atom_info for atom in full atoms
    atom_info fr(int atom) { return full_atoms_[atom]; }
    /// Returns a Vector3 with x, y, z position of atom
    Vector3 xyz(int atom) const { return Vector3(atoms_[atom].x, atoms_[atom].y, atoms_[atom].z); }
    Vector3 fxyz(int atom) const { return Vector3(full_atoms_[atom].x, full_atoms_[atom].y, full_atoms_[atom].z); }
    /// Returns x, y, or z component of 'atom'
    double& xyz(int atom, int _xyz);
    const double& xyz(int atom, int _xyz) const;
    /// Returns mass atom atom
    double mass(int atom) const;
    /// Returns label of atom
    const std::string label(int atom) const;
    /// Returns charge of atom
    double charge(int atom) const { return atoms_[atom].charge; }
    /// Returns mass atom atom
    double fmass(int atom) const { return full_atoms_[atom].mass; }
    /// Returns label of atom
    const std::string flabel(int atom) const { return full_atoms_[atom].label; }
    /// Returns charge of atom
    double fcharge(int atom) const { return full_atoms_[atom].charge; }

    /// Tests to see of an atom is at the passed position with a given tolerance
    int atom_at_position1(double *, double tol = 0.05) const;
    int atom_at_position2(Vector3&, double tol = 0.05) const;

    SimpleMatrix geometry();
    void set_geometry(SimpleMatrix& geom);
    void rotate(SimpleMatrix& R);

    /// Computes center of mass of molecule (does not translate molecule)
    Vector3 center_of_mass() const;
    /// Computes nuclear repulsion energy
    double nuclear_repulsion_energy();
    /// Computes nuclear repulsion energy derivatives. Free with delete[]
    SimpleVector nuclear_repulsion_energy_deriv1();
    /// Computes nuclear repulsion energy second derivatives.
    SimpleMatrix* nuclear_repulsion_energy_deriv2();

    /// Returns the nuclear contribution to the dipole moment
    SimpleVector nuclear_dipole_contribution();
    /// Returns the nuclear contribution to the quadrupole moment
    SimpleVector nuclear_quadrupole_contribution();

    /// Translates molecule by r
    void translate(const Vector3& r);
    /// Moves molecule to center of mass
    void move_to_com();
    /** Reorient molecule to standard frame. See input/reorient.cc
     *  If you want the molecule to be reoriented about the center of mass
     *  make sure you call move_to_com() prior to calling reorient()
     */
    void reorient();

    /// Compute inertia tensor.
    SimpleMatrix* inertia_tensor();
    
    /// Returns the number of irreps
    int nirrep() const { return nirreps_; }
    /// Sets the number of irreps
    void nirrep(int nirreps) { nirreps_ = nirreps; }

    /// Print the molecule
    void print() const;

    /// Save information to checkpoint file.
    void save_to_chkpt(boost::shared_ptr<Chkpt> chkpt, std::string prefix = "");
    
    //
    // Symmetry
    //
    boost::shared_ptr<PointGroup> point_group() const { return pg_; }
    void set_point_group(boost::shared_ptr<PointGroup> pg) { pg_ = pg; }
    /// Does the molecule have an inversion center at origin
    bool has_inversion(Vector3& origin, double tol = 0.05) const;
    /// Is a plane?
    bool is_plane(Vector3& origin, Vector3& uperp, double tol = 0.05) const;
    /// Is an axis?
    bool is_axis(Vector3& origin, Vector3& axis, int order, double tol=0.05) const;
    /// Is the molecule linear, or planar?
    void is_linear_planar(bool& linear, bool& planar, double tol) const;
    /// Find highest molecular point group
    boost::shared_ptr<PointGroup> find_point_group(double tol=1.0e-8) const;
    /// Release symmetry information
    void release_symmetry_information();
    /// Initialize molecular specific symemtry information
    /// Uses the point group object obtain by calling point_group()
    void form_symmetry_information(double tol=1.0e-8);
};

typedef boost::shared_ptr<Molecule> SharedMolecule;

class AtomicRadii
{
protected:
    int count_;
    double *radii_;
public:
    AtomicRadii();
    virtual ~AtomicRadii();

    /** Get the atomic radii.
     *  @param Z atomic number of interest
     *  @returns the atomic radii, or 0 if not found.
     */
    double radii(int Z) const {
        if (Z < 0 || Z >= count_)
            return 0.0;
        return radii_[Z];
    }

    /** Modify the atomic radii. You can only change an existing radii
     *  you cannot add a new one.
     *  @param Z atomic number to modify
     *  @param val new value to use
     */
    void modify_radii(int Z, double val) {
        if (Z < 0 || Z >= count_)
            return;
        radii_[Z] = val;
    }
};

/// Treutler radial mapping radii (see Treutler 1995, Table 1, p. 348).
class TreutlerAtomicRadii : public AtomicRadii
{
public:
    TreutlerAtomicRadii();
};

/** Adjusted van der Waals radii (Angstrom) from atomic ROHF/TZV
 *  computations.
 *  S. Grimme, J. Comput. Chem. 27, 1787-799 (2006)
 */
class AdjustedVanDerWaalsAtomicRadii : public AtomicRadii
{
public:
    AdjustedVanDerWaalsAtomicRadii();
};

/// Bragg-Slater atomic radii
class BraggSlaterAtomicRadii : public AtomicRadii
{
public:
    BraggSlaterAtomicRadii();
};

/** SG1 atomic radii (for use with SG1 grid).
 *  P. M. W. Gill, B. G. Johnson, J. A. Pople, Chem. Phys. Lett.
 *  209, 506 (1993).
 */
class SG1AtomicRadii : public AtomicRadii
{
public:
    SG1AtomicRadii();
};

}

#endif
