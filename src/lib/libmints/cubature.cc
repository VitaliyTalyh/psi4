#include <libmints/mints.h>
#include "cubature.h"
#include "gridblocker.h"
#include <libciomr/libciomr.h>
#include <libqt/qt.h>

#include <string>
#include <sstream>
#include <iostream>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <utility>
#include <limits>
#include <ctype.h>

#include <boost/tuple/tuple.hpp>
#include <boost/tuple/tuple_comparison.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/regex.hpp>
#include <boost/xpressive/xpressive.hpp>
#include <boost/xpressive/regex_actions.hpp>
#include <boost/algorithm/string.hpp>

using namespace boost;
using namespace std;
using namespace psi;

namespace psi {

// the third parameter of from_string() should be
// one of std::hex, std::dec or std::oct
template <class T>
bool from_string(T& t,
                 const std::string& s,
                 std::ios_base& (*f)(std::ios_base&))
{
    std::istringstream iss(s);
    return !(iss >> f >> t).fail();
}

std::map<int,int> SphericalGrid::lebedev_order_to_points_;
std::map<int, boost::shared_ptr<SphericalGrid> > SphericalGrid::lebedev_grids_;
std::vector<double> MolecularGrid::BSRadii_;

BasisExtents::BasisExtents(boost::shared_ptr<BasisSet> primary, double delta) :
    primary_(primary), delta_(delta)
{
    shell_extents_ = boost::shared_ptr<Vector>(new Vector("Shell Extents", primary_->nshell()));
    computeExtents();
}
BasisExtents::~BasisExtents()
{
}
void BasisExtents::computeExtents()
{
    // Here we assume the absolute spherical basis functions
    // |\phi|_P(r) = |C_k| r^l exp(-a_k r^2)
    
    // We are trying to zero the objective function
    // O(r) = |\phi|_P(r) - \delta

    // Bisection is used to avoid accidentally finding the
    // nuclear cusp for p and higher functions

    double* Rp = shell_extents_->pointer();
    maxR_ = 0.0;

    // Compute each shell in turn
    for (int P = 0; P < primary_->nshell(); P++) {

        // Corner case: \delta = 0.0
        if (delta_ == 0.0) {
            Rp[P] = std::numeric_limits<double>::max();
            maxR_ = std::numeric_limits<double>::max();
            continue;
        }
    
        // Stage 1: Collect information on the shell
        boost::shared_ptr<GaussianShell> Pshell = primary_->shell(P);
        int l         = Pshell->am();
        int nprim     = Pshell->nprimitive();      
        double* alpha = Pshell->exps();
        double* norm  = Pshell->coefs();

        double norm_max = norm[0];
        double alpha_max = alpha[0];

        // Find most-diffuse primitive
        for (int K = 0; K < nprim; K++) {
            if (alpha_max > alpha[K]) {
                alpha_max = alpha[K];
                norm_max = fabs(norm[K]);
            }    
        } 

        // Stage 2: Form a well-posed bounding box for R
        
        // A: Force O at the right end of the box to be negative
       
        // Take a crude guess based on most-diffuse exponent
        // Removing nonlinearity with R0 = 10.0 a.u.
        double Rr = 2.0;
        double Or = 0.0;
        do {
            // Compute Or
            Or = 0.0;
            for (int K = 0; K < nprim; K++) {
                Or += fabs(norm[K]) * pow(Rr,l) * exp(-alpha[K] * Rr * Rr);
            }        
            Or = fabs(Or) - delta_; 
           
            // Move further right 
            if (Or > 0.0) 
                Rr *= 2.0;
 
        } while (Or > 0.0);

        // B: Force O at the left end of the box to be positive
        double Rl = Rr; // We know this is negative due to Rr 
        double Ol = 0.0;
        do {
            // Compute Ol
            Ol = 0.0;
            for (int K = 0; K < nprim; K++) {
                Ol += fabs(norm[K]) * pow(Rl,l) * exp(-alpha[K] * Rl * Rl);
            }       
            Ol = fabs(Ol) - delta_; 
            
            // Move further left
            if (Ol < 0.0) 
                Rl /= 2.0;

            // Check if we missed the positive bit in the middle somehow
            if (fabs(Rl) == 0.0) {
                throw PSIEXCEPTION("BasisExtents: Left root of basis cutoffs found the nuclear cusp.\n"
                    "This is very bad.");
            }
 
        } while (Ol < 0.0);

        // Stage 3: Locate R via bisection
        double Rc, Oc;
        do {
            Rc = 0.5 * (Rl + Rr);
            Oc = 0.0;
            for (int K = 0; K < nprim; K++) {
                Oc += fabs(norm[K]) * pow(Rc,l) * exp(-alpha[K] * Rc * Rc);
            }
            Oc = fabs(Oc) - delta_;
            
            if (Oc > 0.0) {
                Rl = Rc;
                Ol = Oc;
            } else {
                Rr = Rc;
                Or = Oc;
            }           
            // My MechE profs would disapprove of this cutoff.  
        } while (fabs(Rr - Rl) > 1.0E-8 * Rl && fabs(Oc) != 0.0);
    
        // Assign the calculated value
        Rp[P] = Rc;

        // Keep track of the maximum extent
        if (maxR_ < Rc) {
            maxR_ = Rc;
        }
    }
}
void BasisExtents::print(FILE* out)
{
    fprintf(out, "   => BasisExtents: Cutoff = %11.3E <=\n\n", delta_);

    double* Rp = shell_extents_->pointer();
    fprintf(out, "   Shell Extents:\n"); 
    fprintf(out, "   %4s %14s %14s %14s %14s\n", "N", "X", "Y", "Z", "R");
    for (int Q = 0; Q < primary_->nshell(); Q++) {
        Vector3 v = primary_->shell(Q)->center();
        fprintf(out, "   %4d %14.6E %14.6E %14.6E %14.6E\n", Q+1, 
            v[0], v[1], v[2], Rp[Q]); 
    } 
    fprintf(out, "\n\n");
}
BlockOPoints::BlockOPoints(int npoints, double* x, double* y, double* z, double* w, boost::shared_ptr<BasisExtents> extents) :
    npoints_(npoints), x_(x), y_(y), z_(z), w_(w), extents_(extents)
{
    bound();
    populate();
}
BlockOPoints::~BlockOPoints()
{
}
void BlockOPoints::bound()
{
    // Initially: mean center and max spread of point cloud 
    double R2 = 0.0;

    xc_[0] = xc_[1] = xc_[2] = R2 = 0.0;

    for (int Q = 0; Q < npoints_; Q++) {
        xc_[0] += x_[Q];
        xc_[1] += y_[Q];
        xc_[2] += z_[Q];
    } 

    xc_[0] /= (double) npoints_;
    xc_[1] /= (double) npoints_;
    xc_[2] /= (double) npoints_;

    for (int Q = 0; Q < npoints_; Q++) {
        double R2_candidate = (x_[Q] - xc_[0]) * (x_[Q] - xc_[0]) +
                              (y_[Q] - xc_[1]) * (y_[Q] - xc_[1]) +
                              (z_[Q] - xc_[2]) * (z_[Q] - xc_[2]);
        if (R2 < R2_candidate)
            R2 = R2_candidate;
    }

    R_ = sqrt(R2);
}
void BlockOPoints::populate()
{
    shells_local_to_global_.clear();
    functions_local_to_global_.clear();

    boost::shared_ptr<BasisSet> primary = extents_->basis();
    double* Rp = extents_->shell_extents()->pointer();

    // Determine significant shell/functions 
    for (int P = 0; P < primary->nshell(); P++) {
       
        // First pass: mean center/max spread of point clould 
        Vector3 v = primary->shell(P)->center();
        double Reff = sqrt((v[0] - xc_[0]) * (v[0] - xc_[0]) +
                           (v[1] - xc_[1]) * (v[1] - xc_[1]) +
                           (v[2] - xc_[2]) * (v[2] - xc_[2]));
        
        
        if (Reff > R_ + Rp[P]) continue;

        // Second pass: check individual points
        double Rp2 = Rp[P] * Rp[P];
        for (int Q = 0; Q < npoints_; Q++) {
            double R2_candidate = (x_[Q] - v[0]) * (x_[Q] - v[0]) +
                                  (y_[Q] - v[1]) * (y_[Q] - v[1]) +
                                  (z_[Q] - v[2]) * (z_[Q] - v[2]);
            if (R2_candidate < Rp2) {
                // Sig Shell located!
                int nP = primary->shell(P)->nfunction();
                int pstart = primary->shell(P)->function_index();

                shells_local_to_global_.push_back(P);
                for (int oP = 0; oP < nP; oP++) {
                    functions_local_to_global_.push_back(oP + pstart);
                } 

                break;
            } 
        }
    }
}
void BlockOPoints::print(FILE* out, int print)
{
    fprintf(out, "   => BlockOPoints: %d Points <=\n\n", npoints_);
    
    fprintf(out, "    Center = <%11.3E,%11.3E,%11.3E>, R = %11.3E\n\n",
        xc_[0],xc_[1],xc_[2],R_);

    fprintf(out, "    %-6lu Significant Shells.\n", shells_local_to_global_.size()); 
    fprintf(out, "    %-6lu Significant Functions.\n\n", functions_local_to_global_.size());

    if (print > 3) {
        fprintf(out, "    Significant Shells: ");
        for (int i = 0; i < shells_local_to_global_.size(); i++) {
            fprintf(out, "%d ", shells_local_to_global_[i]);
        } 
        fprintf(out, "\n\n");
        fprintf(out, "    Significant Functions: ");
        for (int i = 0; i < functions_local_to_global_.size(); i++) {
            fprintf(out, "%d ", functions_local_to_global_[i]);
        } 
        fprintf(out, "\n\n");
    }

    if (print > 5) {

        fprintf(out, "   Quadrature Points:\n\n");
        fprintf(out, "   %4s %14s %14s %14s %14s\n", "N", "X", "Y", "Z", "W");
        for (int i = 0; i < npoints_; i++) {
            fprintf(out, "   %4d %14.6E %14.6E %14.6E %14.6E\n", i+1, 
                x_[i], y_[i], z_[i], w_[i]); 
        } 
        fprintf(out, "\n\n");
    } 
}
DFTGrid::DFTGrid(boost::shared_ptr<Molecule> molecule,
                 boost::shared_ptr<BasisSet> primary,
                 Options& options) :
    MolecularGrid(molecule), primary_(primary), options_(options)
{
    buildGridFromOptions();
}
DFTGrid::~DFTGrid()
{
}
void DFTGrid::buildGridFromOptions()
{
    SharedMatrix rotation = standard_orientation(molecule_);

    std::vector<boost::shared_ptr<AtomicGrid> > atoms;

    boost::shared_ptr<Vector> alpha(new Vector("Alpha Center per Atom", molecule_->natom()));
    double* alphap = alpha->pointer(); 

    MolecularGrid::getBSRadii();
    for (int A = 0; A < molecule_->natom(); A++) {
        alphap[A] = BSRadii_[molecule_->Z(A)] * options_.get_double("DFT_BS_RADIUS_ALPHA");
    }

    std::map<int, int> leb_orders = SphericalGrid::lebedevOrdersToPoints();
    std::vector<int> valid_orders;

    for (std::map<int, int>::iterator it = leb_orders.begin(); it != leb_orders.end(); it++) {
        if ((*it).first > 1) // Order 1 is for trial purposes only
            valid_orders.push_back((*it).first);
    } 

    std::sort(valid_orders.begin(), valid_orders.end());

    if (options_.get_str("DFT_GRID_NAME") != "") {
        throw FeatureNotImplemented("DFTGrid","Named Grids, e.g., SG1",__FILE__,__LINE__); 
        // TODO: Named grids
    }

    for (int A = 0; A < molecule_->natom(); A++) {
        boost::shared_ptr<RadialGrid> radial = RadialGrid::buildGrid(
            options_.get_str("DFT_RADIAL_SCHEME"), 
            options_.get_int("DFT_N_RADIAL"),
            alphap[A]);
        std::vector<boost::shared_ptr<SphericalGrid> > spheres;
        for (int i = 0; i < radial->npoints(); i++) {
            if (options_.get_str("DFT_PRUNING_SCHEME") == "FLAT") {
                spheres.push_back(SphericalGrid::buildGrid(
                    options_.get_str("DFT_SPHERICAL_SCHEME"),
                    options_.get_int("DFT_ORDER_SPHERICAL")));
            } else if (options_.get_str("DFT_PRUNING_SCHEME") == "P_SLATER") {

                double exponent = 1.0 / (alphap[A]);
                double Ap = ((double) options_.get_int("DFT_ORDER_SPHERICAL")) * exp(1.0) / (alphap[A]);
                double R = radial->r()[i];

                int Ldesired = (int) ceil(Ap * R * exp(-exponent * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("DFT_PRUNING_SCHEME") == "D_SLATER") {

                double exponent = 2.0 / (alphap[A]);
                double Ap = ((double) options_.get_int("DFT_ORDER_SPHERICAL")) * exp(2.0) / (alphap[A] * alphap[A]);
                double R = radial->r()[i];

                int Ldesired = (int) ceil(Ap * R * R * exp(-exponent * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("DFT_PRUNING_SCHEME") == "LOG_SLATER") {

                double exponent = options_.get_double("DFT_PRUNING_ALPHA");
                double Ap = ((double) options_.get_int("DFT_ORDER_SPHERICAL"));
                double R = log(radial->r()[i] / alphap[A]);

                int Ldesired = (int) ceil(Ap * exp(-exponent * fabs(R)) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("DFT_PRUNING_SCHEME") == "P_GAUSSIAN") {

                double exponent = 1.0 / (2.0 * alphap[A]);
                double Ap = ((double) options_.get_int("DFT_ORDER_SPHERICAL")) * exp(1.0 / 2.0) / (alphap[A]);
                double R = radial->r()[i];

                int Ldesired = (int) ceil(Ap * R * exp(-exponent * R * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("DFT_PRUNING_SCHEME") == "D_GAUSSIAN") {

                double exponent = 1.0 / (alphap[A] * alphap[A]);
                double Ap = ((double) options_.get_int("DFT_ORDER_SPHERICAL")) * exp(1.0) / (alphap[A] * alphap[A]);
                double R = radial->r()[i];

                int Ldesired = (int) ceil(Ap * R * R * exp(-exponent * R * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("DFT_PRUNING_SCHEME") == "LOG_GAUSSIAN") {

                double exponent = options_.get_double("DFT_PRUNING_ALPHA");
                double Ap = ((double) options_.get_int("DFT_ORDER_SPHERICAL"));
                double R = log(radial->r()[i] / alphap[A]);

                int Ldesired = (int) ceil(Ap * exp(-exponent * R * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            }
        }
        boost::shared_ptr<AtomicGrid> atom(new AtomicGrid());
        atom->buildGrid(molecule_->xyz(A),
            rotation,
            radial,
            spheres);
        atoms.push_back(atom);
    }

    // Blocking/sieving info
    int max_points = options_.get_int("DFT_MAX_POINTS");
    int min_points = options_.get_int("DFT_MIN_POINTS");
    double epsilon = options_.get_double("DFT_BASIS_CUTOFF");
    boost::shared_ptr<BasisExtents> extents(new BasisExtents(primary_, epsilon));

    /// Apply nuclear weights
    buildGrid(atoms, options_.get_str("DFT_NUCLEAR_SCHEME"), extents, max_points, min_points);     
}


PseudospectralGrid::PseudospectralGrid(boost::shared_ptr<Molecule> molecule,
                                       boost::shared_ptr<BasisSet> primary,
                                       boost::shared_ptr<BasisSet> dealias,
                                       Options& options) :
    MolecularGrid(molecule), primary_(primary), dealias_(dealias), filename_(""),  options_(options)
{
    buildGridFromOptions();
}
PseudospectralGrid::PseudospectralGrid(boost::shared_ptr<Molecule> molecule,
                                       boost::shared_ptr<BasisSet> primary,
                                       boost::shared_ptr<BasisSet> dealias,
                                       const std::string& filename,
                                       Options& options) :
    MolecularGrid(molecule), primary_(primary), dealias_(dealias), filename_(filename), options_(options)
{
    buildGridFromFile();
}
PseudospectralGrid::~PseudospectralGrid()
{
} 
void PseudospectralGrid::buildGridFromFile()
{
    throw FeatureNotImplemented("PseudospectralGrid","buildGridFromFile",__FILE__,__LINE__);
/**
    std::string PSIDATADIR = Process::environment("PSIDATADIR");
    std::string gridname = PSIDATADIR + "grids/" + filename_ + ".grid";

    //cout << gridname;

    // Phase I: read the grid in
    std::vector<std::vector<std::pair<int, std::pair<double, double> > > > array;
    array.resize(molecule_->natom());

    regex comment("^\\s*\\!.*");                                       // line starts with !
    regex separator("^\\*\\*\\*\\*");                                  // line starts with ****
    regex atom_array("^\\s*([A-Za-z]+)\\s+0.*");                       // array of atomic symbols terminated by 0

#define NUMBER "((?:[-+]?\\d*\\.\\d+(?:[DdEe][-+]?\\d+)?)|(?:[-+]?\\d+\\.\\d*(?:[DdEe][-+]?\\d+)?))"

    regex grid_shell("^\\s*(\\d+)\\s+" NUMBER ".*"); // match
    // Hold the result of a regex_match
    smatch what;

    std::vector<std::string> lines;
    std::string text;
    ifstream infile(gridname.c_str());
    if (!infile)
        throw PSIEXCEPTION("PseudoGridParser::parse: Unable to open pseudospectral grid file: " + filename);
    while (infile.good()) {
        getline(infile, text);
        lines.push_back(text);
    }

    for (int atom=0; atom<molecule_->natom(); ++atom) {

        int lineno = 0;
        bool found = false;
        while (lineno < lines.size()) {
            string line = lines[lineno++];

            // Spit out the line for debugging.
            //cout << line << endl;

            // Ignore blank lines
            if (line.empty())
                continue;

            // Do some matches
            if (regex_match(line, what, comment)) {
                //cout << " matched a comment line.\n";
                continue;
            }
            if (regex_match(line, what, separator)) {
                //cout << " matched a separator line.\n";
                continue;
            }
            // Match: H    0
            // or:    H    O...     0
            if (regex_match(line, what, atom_array)) {
                //cout << " matched a atom array line.\n";
                //cout << " what.captures(1)" << what[1].str() << "\n";

                // Check the captures and see if this basis set is for the atom we need.
                if (iequals(molecule_->label(atom), what[1].str())) {
                    found = true;
                    line = lines[lineno++];

                    // Need to do the following until we match a "****" which is the end of the basis set
                    while (!regex_match(line, what, separator)) {
                        // cout << " Atom line " << line;
                        if (regex_match(line, what, grid_shell)) {
                            int L;
                            double r;
                            if (!from_string<int>(L, what[1], std::dec))
                                throw PSIEXCEPTION("PseudoGridParser::parse: Unable to convert number of points (order):\n" + line);
                            if (!from_string<double>(r, what[2], std::dec))
                                throw PSIEXCEPTION("PseudoGridParser::parse: Unable to convert grid shell radius:\n" + line);
                            array[atom].push_back(make_pair(L, make_pair(r,1.0)));
                        }
                        line = lines[lineno++];
                    }
                    break;
                } else {
                    continue;
                }
            } else {
                continue;
            }

        }
        if (found == false)
            throw PSIEXCEPTION("PseudoGridParser::parser: Unable to find the grid for " + molecule_->label(atom));
    }

    std::map<int,int> lebedev_orders_to_points = SphericalGrid::lebedevOrdersToPoints(); 

    int npoints = 0;
    for (int atom = 0; atom < molecule_->natom(); atom++) {

        for (std::vector<std::pair<int, std::pair<double, double> > >::iterator it = array[atom].begin(); it != array[atom].end(); it++) {
            std::pair<int, std::pair<double,double> > row = (*it);
            int L = row.first;

            if (lebedev_orders_to_points.count(L) == 0)
                throw PSIEXCEPTION("Grid order does not match available Lebedev grid orders");

            npoints += lebedev_orders_to_points[L];
        }
    }

    if (npoints == 0)
        throw PSIEXCEPTION("PseudoGridParser: No Grid points in this molecule");


    std::vector<AtomicGrid> atoms;    

    // Phase II: build the grid
    for (int atom = 0; atom < molecule_->natom(); atom++) {

        Vector3 center = molecule_->xyz(atom);
        double xc = center[0];
        double yc = center[1];
        double zc = center[2];

        for (std::vector<std::pair<int, std::pair<double, double> > >::iterator it = array[atom].begin(); it != array[atom].end(); it++) {
            std::pair<int, std::pair<double,double> > row = (*it);
            int L = row.first;
            double r = row.second.first;
            double w = row.second.second;

            int ngrid = 0;

            // Defined in integrator_defines.h
            int max_grid = n_lebedev_;
            for (int index = 0; index < max_grid; index++) {
                if (lebedev_orders_[index] == L)
                    ngrid = lebedev_points_[index];
            }
            if (ngrid == 0)
                throw PSIEXCEPTION("Grid order does not match available Lebedev grid orders");

            SphericalQuadrature quad = integrator->getLebedevSpherical(ngrid);

            for (int p = 0; p < ngrid; p++) {
                xp[counter + p] = quad.x[p];
                yp[counter + p] = quad.y[p];
                zp[counter + p] = quad.z[p];
                //wp[counter + p] = quad.w[p];
                wp[counter + p] = w;
            }
            free(quad.x);
            free(quad.y);
            free(quad.z);
            free(quad.w);

            // Scale to radius
            for (int l = 0; l < ngrid; l++) {
                xp[counter + l] *= r;
                yp[counter + l] *= r;
                zp[counter + l] *= r;
//                wp[counter + l] *= r*r;
            }

            // Center
            for (int l = 0; l < ngrid; l++) {
                xp[counter + l] += xc;
                yp[counter + l] += yc;
                zp[counter + l] += zc;
            }

            // TODO rotate to standard orientation
            // TODO scale for radial weight
            // TODO account for atomic cells

            counter += ngrid;
        }
    }
**/
}
void PseudospectralGrid::buildGridFromOptions()
{
    SharedMatrix rotation = standard_orientation(molecule_);

    std::vector<boost::shared_ptr<AtomicGrid> > atoms;

    boost::shared_ptr<Vector> alpha(new Vector("Alpha Center per Atom", molecule_->natom()));
    double* alphap = alpha->pointer(); 

    MolecularGrid::getBSRadii();
    for (int A = 0; A < molecule_->natom(); A++) {
        alphap[A] = BSRadii_[molecule_->Z(A)] * options_.get_double("PS_BS_RADIUS_ALPHA");
    }

    std::map<int, int> leb_orders = SphericalGrid::lebedevOrdersToPoints();
    std::vector<int> valid_orders;

    for (std::map<int, int>::iterator it = leb_orders.begin(); it != leb_orders.end(); it++) {
        if ((*it).first > 1) // Order 1 is for trial purposes only
            valid_orders.push_back((*it).first);
    } 

    std::sort(valid_orders.begin(), valid_orders.end());

    for (int A = 0; A < molecule_->natom(); A++) {
        boost::shared_ptr<RadialGrid> radial = RadialGrid::buildGrid(
            options_.get_str("PS_RADIAL_SCHEME"), 
            options_.get_int("PS_N_RADIAL"),
            alphap[A]);
        std::vector<boost::shared_ptr<SphericalGrid> > spheres;
        for (int i = 0; i < radial->npoints(); i++) {
            if (options_.get_str("PS_PRUNING_SCHEME") == "FLAT") {
                spheres.push_back(SphericalGrid::buildGrid(
                    options_.get_str("PS_SPHERICAL_SCHEME"),
                    options_.get_int("PS_ORDER_SPHERICAL")));
            } else if (options_.get_str("PS_PRUNING_SCHEME") == "P_SLATER") {

                double exponent = 1.0 / (alphap[A]);
                double Ap = ((double) options_.get_int("PS_ORDER_SPHERICAL")) * exp(1.0) / (alphap[A]);
                double R = radial->r()[i];

                int Ldesired = (int) ceil(Ap * R * exp(-exponent * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("PS_PRUNING_SCHEME") == "D_SLATER") {

                double exponent = 2.0 / (alphap[A]);
                double Ap = ((double) options_.get_int("PS_ORDER_SPHERICAL")) * exp(2.0) / (alphap[A] * alphap[A]);
                double R = radial->r()[i];

                int Ldesired = (int) ceil(Ap * R * R * exp(-exponent * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("PS_PRUNING_SCHEME") == "LOG_SLATER") {

                double exponent = options_.get_double("PS_PRUNING_ALPHA");
                double Ap = ((double) options_.get_int("PS_ORDER_SPHERICAL"));
                double R = log(radial->r()[i] / alphap[A]);

                int Ldesired = (int) ceil(Ap * exp(-exponent * fabs(R)) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("PS_PRUNING_SCHEME") == "P_GAUSSIAN") {

                double exponent = 1.0 / (2.0 * alphap[A]);
                double Ap = ((double) options_.get_int("PS_ORDER_SPHERICAL")) * exp(1.0 / 2.0) / (alphap[A]);
                double R = radial->r()[i];

                int Ldesired = (int) ceil(Ap * R * exp(-exponent * R * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("PS_PRUNING_SCHEME") == "D_GAUSSIAN") {

                double exponent = 1.0 / (alphap[A] * alphap[A]);
                double Ap = ((double) options_.get_int("PS_ORDER_SPHERICAL")) * exp(1.0) / (alphap[A] * alphap[A]);
                double R = radial->r()[i];

                int Ldesired = (int) ceil(Ap * R * R * exp(-exponent * R * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            } else if (options_.get_str("PS_PRUNING_SCHEME") == "LOG_GAUSSIAN") {

                double exponent = options_.get_double("PS_PRUNING_ALPHA");
                double Ap = ((double) options_.get_int("PS_ORDER_SPHERICAL"));
                double R = log(radial->r()[i] / alphap[A]);

                int Ldesired = (int) ceil(Ap * exp(-exponent * R * R) - 1.0E-10); 
    
                int Lselected = 0;

                for (int j = 0; j < valid_orders.size(); j++) {
                    if (Ldesired <= valid_orders[j]) {
                        Lselected = valid_orders[j];
                        break;
                    }
                }

                if (Lselected == 0)
                    throw PSIEXCEPTION("PseudospectralGrid: Requested Spherical Order is too high in pruned grid");

                // Must use lebedev for pruned grids 
                spheres.push_back(SphericalGrid::buildGrid(
                    "LEBEDEV",
                    Lselected));
            }
        }
        boost::shared_ptr<AtomicGrid> atom(new AtomicGrid());
        atom->buildGrid(molecule_->xyz(A),
            rotation,
            radial,
            spheres);
        atoms.push_back(atom);
    }

    int max_points = options_.get_int("PS_MAX_POINTS");
    int min_points = options_.get_int("PS_MIN_POINTS");
    double epsilon = options_.get_double("PS_BASIS_CUTOFF");
    boost::shared_ptr<BasisExtents> extents(new BasisExtents(primary_, epsilon));

    /// Apply nuclear weights
    buildGrid(atoms, options_.get_str("PS_NUCLEAR_SCHEME"), extents, max_points, min_points);     
}

MolecularGrid::MolecularGrid(boost::shared_ptr<Molecule> molecule) :
    molecule_(molecule), npoints_(0), max_points_(0), max_functions_(0), scheme_("")
{
}
MolecularGrid::~MolecularGrid()
{
    if (npoints_) {
        delete[] x_;
        delete[] y_;
        delete[] z_;
        delete[] w_;
    }
}
void MolecularGrid::sieve()
{
    // => Remove points with 0.0 weight <= //
    remove_zero_points();    

    // => Remove points that are very distant <= //
    remove_distant_points(extents_->maxR());
}
void MolecularGrid::block(int max_points, int min_points)
{
    // Hack
    Options& options_ = Process::environment.options;

    // Reassign
    boost::shared_ptr<GridBlocker> blocker;
    if (options_.get_str("DFT_BOXING_SCHEME") == "NAIVE") {
        blocker = boost::shared_ptr<GridBlocker>(new NaiveGridBlocker(npoints_,x_,y_,z_,w_,max_points,min_points,extents_));
    } else if (options_.get_str("DFT_BOXING_SCHEME") == "OCTREE") {
        blocker = boost::shared_ptr<GridBlocker>(new OctreeGridBlocker(npoints_,x_,y_,z_,w_,max_points,min_points, extents_));
    }

    blocker->set_print(options_.get_int("PRINT"));
    blocker->set_debug(options_.get_int("DEBUG"));
    blocker->set_bench(options_.get_int("BENCH"));

    blocker->block();

    delete[] x_;
    delete[] y_;
    delete[] z_;
    delete[] w_;

    x_ = blocker->x();
    y_ = blocker->y();
    z_ = blocker->z();
    w_ = blocker->w();

    npoints_ = blocker->npoints();
    max_points_ = blocker->max_points();
    max_functions_ = blocker->max_functions();

    const std::vector<boost::shared_ptr<BlockOPoints> >& block = blocker->blocks();
    for (int i = 0; i < block.size(); i++) {
        blocks_.push_back(block[i]);
    }
}
void MolecularGrid::remove_zero_points()
{
    int npoints2 = npoints_;
    int offset = 0;
    for (int Q = 0; Q < npoints_; Q++) {
        if (w_[Q] == 0.0) {
            npoints2--;
        } else {
            x_[offset] = x_[Q];
            y_[offset] = y_[Q];
            z_[offset] = z_[Q];
            w_[offset] = w_[Q];
            offset++;
        }
    }
    npoints_ = npoints2;
}
void MolecularGrid::remove_distant_points(double Rmax)
{
    if (Rmax == std::numeric_limits<double>::max())
        return;

    int natom = molecule_->natom();
    int npoints2 = npoints_;
    int offset = 0;
    for (int Q = 0; Q < npoints_; Q++) {
        Vector3 v = molecule_->xyz(0);
        double R = (x_[Q] - v[0]) * (x_[Q] - v[0]) + 
                   (y_[Q] - v[1]) * (y_[Q] - v[1]) + 
                   (z_[Q] - v[2]) * (z_[Q] - v[2]);  
        for (int A = 1; A < natom; A++) {
            v = molecule_->xyz(A);
            double R2 = (x_[Q] - v[0]) * (x_[Q] - v[0]) + 
                        (y_[Q] - v[1]) * (y_[Q] - v[1]) + 
                        (z_[Q] - v[2]) * (z_[Q] - v[2]);  
            if (R > R2) {
                R = R2;
            }
        }

        if (R > Rmax * Rmax) {
            npoints2--;
        } else {
            x_[offset] = x_[Q];
            y_[offset] = y_[Q];
            z_[offset] = z_[Q];
            w_[offset] = w_[Q];
            offset++;
        }
    }
    npoints_ = npoints2;
}
SharedMatrix MolecularGrid::standard_orientation(boost::shared_ptr<Molecule> mol) 
{
    SharedMatrix R(new Matrix("Standard Orientation", 3, 3));
    int natom = mol->natom();

    // These rules are based on the industry-standard, and entirely
    // inelegant rules of Gill, Johnson, and Pople, Chem Phys Lett
    // 209(5), 1993, pp. 511    

    // Center of nuclear charge
    double Z = 0.0;
    SharedVector X(new Vector("Charge Center", 3));
    double *Xp = X->pointer();

    for (int A = 0; A < natom; A++) {
        Xp[0] += mol->x(A) * mol->Z(A);
        Xp[1] += mol->y(A) * mol->Z(A);
        Xp[2] += mol->z(A) * mol->Z(A);
        Z += mol->Z(A);
    }

    Xp[0] /= Z; 
    Xp[1] /= Z; 
    Xp[2] /= Z; 
    
    // Second moment of charge tensor
    SharedMatrix I(new Matrix("Second Moment of Charge", 3, 3));
    double** Ip = I->pointer();
    
    for (int A = 0; A < natom; A++) {
        Ip[0][0] += (mol->x(A) - Xp[0]) * (mol->x(A) - Xp[0]) * mol->Z(A);
        Ip[0][1] += (mol->x(A) - Xp[0]) * (mol->y(A) - Xp[1]) * mol->Z(A);
        Ip[0][2] += (mol->x(A) - Xp[0]) * (mol->z(A) - Xp[2]) * mol->Z(A);
        Ip[1][1] += (mol->y(A) - Xp[1]) * (mol->y(A) - Xp[1]) * mol->Z(A);
        Ip[1][2] += (mol->y(A) - Xp[1]) * (mol->z(A) - Xp[2]) * mol->Z(A);
        Ip[2][2] += (mol->z(A) - Xp[2]) * (mol->z(A) - Xp[2]) * mol->Z(A);
    }
    Ip[1][0] = Ip[0][1];
    Ip[2][0] = Ip[0][2];
    Ip[2][1] = Ip[1][2];

    // Principal axes of charge tensor
    SharedMatrix I2(I->clone());
    SharedMatrix V(new Matrix("Principal Axes", 3, 3));
    SharedVector D(new Vector("Principal Moments", 3));

    I2->diagonalize(V,D);

    fprintf(outfile, "  => MolecularGrid: Standard Orientation <= \n\n");
    fprintf(outfile, "  Total Charge: %d\n\n", (int) Z); 
    X->print();
    I->print();
    D->print();
    V->print();

    // What kinda bug are we dealing with here?
    
    // Jitter tolerance
    double* Dp = D->pointer();
    double** Vp = V->pointer();
    double jitter = 1.0E-6;
    if (Dp[0] < jitter && Dp[1] < jitter && Dp[2] < jitter) {
        // Spherical top, single atom
        fprintf(outfile, "    Spherical top, single atom\n\n");
        R->identity();
    } else if (Dp[0] < jitter && Dp[1] < jitter) {
        // Symmetric top, linear
        fprintf(outfile, "    Spherical top, linear\n\n");
        R->copy(V);
    } else if (fabs(Dp[0] - Dp[1]) < jitter && fabs(Dp[1] - Dp[2]) < jitter) {
        // Spherical top, polyatomic
        fprintf(outfile, "    Spherical top, polyatomic\n\n");
        // Case 1 Icosohedral
        // Case 2 Octohedral
        // Case 3 Tetrahedral
        // Hack TODO Temporary

        // Find a significant atom
        double v1[3];
        int index = -1;
        for (int A = 0; A < natom; A++) {
            v1[0] = mol->x(A) - Xp[0];
            v1[1] = mol->y(A) - Xp[1];
            v1[2] = mol->z(A) - Xp[2];

            double norm = sqrt(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
        
            if (norm > jitter) {
                v1[0] /= norm;
                v1[1] /= norm;
                v1[2] /= norm;
                index = A;
                break;
            }
        }
        if (!index) throw PSIEXCEPTION("No relevant atom found in tetrahedral or higher symmetry");

        // And another
        double v2[3];
        int index2 = -1;
        for (int A = index+1; A < natom; A++) {
            v2[0] = mol->x(A) - Xp[0];
            v2[1] = mol->y(A) - Xp[1];
            v2[2] = mol->z(A) - Xp[2];

            double norm = sqrt(v2[0] * v2[0] + v2[1] * v2[1] + v2[2] * v2[2]);
        
            if (norm > jitter) {
                v2[0] /= norm;
                v2[1] /= norm;
                v2[2] /= norm;

                // Check for collinearity
                if (fabs(v1[0]*v2[0] + v1[1]*v2[1] + v1[2]*v2[2]) < 1.0-jitter) {
                    index2 = A;
                    break;
                }
            }
        }
        if (!index2) throw PSIEXCEPTION("No relevant atom found in tetrahedral or higher symmetry");
        
        // Householder to place significant atom 1 in Z or -Z (choice is arbitrary, but more stable)
        double** Rp = R->pointer();

        double alpha = (v1[2] > 0.0 ? -1.0 : 1.0);
        double V[3];
        V[0] = v1[0];
        V[1] = v1[1];
        V[2] = v1[2] + alpha;
        double norm2 = V[0] * V[0] + V[1] * V[1] + V[2] * V[2]; 
        
        R->identity();    
        Rp[0][0] -= 2.0/norm2 * V[0] * V[0];
        Rp[0][1] -= 2.0/norm2 * V[0] * V[1];
        Rp[0][2] -= 2.0/norm2 * V[0] * V[2];
        Rp[1][0] -= 2.0/norm2 * V[1] * V[0];
        Rp[1][1] -= 2.0/norm2 * V[1] * V[1];
        Rp[1][2] -= 2.0/norm2 * V[1] * V[2];
        Rp[2][0] -= 2.0/norm2 * V[2] * V[0];
        Rp[2][1] -= 2.0/norm2 * V[2] * V[1];
        Rp[2][2] -= 2.0/norm2 * V[2] * V[2];

        // Givens rotation in X and Y to place atom 2 in X
        double v3[3];
        v3[0] = Rp[0][0] * v2[0] + Rp[0][1] * v2[1] + Rp[0][2] * v2[2];  
        v3[1] = Rp[1][0] * v2[0] + Rp[1][1] * v2[1] + Rp[1][2] * v2[2];  
        v3[2] = Rp[2][0] * v2[0] + Rp[2][1] * v2[1] + Rp[2][2] * v2[2];  

        double norm = sqrt(v3[0] * v3[0] + v3[1] * v3[1]);
        v3[0] /= norm;
        v3[1] /= norm;
        v3[2] /= norm;
        
        double c = v3[0]; 
        double s = v3[1];

        double xc, yc;
        xc =  c * Rp[0][0] + s * Rp[1][0];
        yc = -s * Rp[0][0] + c * Rp[1][0];
        Rp[0][0] = xc;
        Rp[1][0] = yc;

        xc =  c * Rp[0][1] + s * Rp[1][1];
        yc = -s * Rp[0][1] + c * Rp[1][1];
        Rp[0][1] = xc;
        Rp[1][1] = yc;

        xc =  c * Rp[0][2] + s * Rp[1][2];
        yc = -s * Rp[0][2] + c * Rp[1][2];
        Rp[0][2] = xc;
        Rp[1][2] = yc;

    } else if (fabs(Dp[0] - Dp[1]) < jitter || fabs(Dp[1] - Dp[2]) < jitter) {
        // Symmetric top, nonlinear
        fprintf(outfile, "    Symmetric top, nonlinear\n\n");
    
        // Put the unique axis in z
        if (fabs(Dp[1] - Dp[2]) < jitter) {
            double temp = Dp[0];
            Dp[0] = Dp[1];
            Dp[1] = Dp[2];
            Dp[2] = temp;

            double tempp[3];
            C_DCOPY(3, &Vp[0][0], 3, tempp, 1);
            C_DCOPY(3, &Vp[0][1], 3, &Vp[0][0], 3);
            C_DCOPY(3, &Vp[0][2], 3, &Vp[0][1], 3);
            C_DCOPY(3, tempp, 1, &Vp[0][2], 3);
        }
    
        // Develop projection onto Z and R for each atom
        SharedVector Zproj(new Vector("Projection on Z", natom));
        SharedVector Rproj(new Vector("Projection on R", natom));
        double* Zp = Zproj->pointer();
        double* Rp = Rproj->pointer();
        
        double v[3];
        for (int A = 0; A < natom; A++) {
            v[0] = mol->x(A) - Xp[0];
            v[1] = mol->y(A) - Xp[1];
            v[2] = mol->z(A) - Xp[2];

            Zp[A] = v[0] * Vp[0][2] +     
                    v[1] * Vp[1][2] +     
                    v[2] * Vp[2][2];     

            Rp[A] = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2] - Zp[A] * Zp[A]);
        }

        //Zproj->print();
        //Rproj->print();

        // Eliminate non-significant digits (below jitter) in Z and R to prevent false sorting
        for (int A = 0; A < natom; A++) {
            if (Zp[A] > 0) { 
                Zp[A] = jitter * round(Zp[A] / jitter);
            } else {
                Zp[A] = -jitter * round(-Zp[A] / jitter);
            }
            if (Rp[A] > 0) { 
                Rp[A] = jitter * round(Rp[A] / jitter);
            } else {
                Rp[A] = -jitter * round(-Rp[A] / jitter);
            }
        }

        // Develop tests 3a -> 3d
        // tuple is fabs(Zp), -sgn(Zp), fabs(Rp), Z, index
        std::vector<boost::tuple<double, int, double, int, int> > tests;
        for (int A = 0; A < natom; A++) {
            tests.push_back(boost::tuple<double,int,double,int,int>(fabs(Zp[A]), (int) (Zp[A] < 0) - (int) (Zp[A] > 0), Rp[A], mol->Z(A), A));
        }
    
        std::sort(tests.begin(), tests.end());

        // Set index of projection
        int index = 0;
        for (int A = 0; A < natom; A++) {
            double Rp = boost::get<2>(tests[A]);
            if (Rp > jitter) {
                index = boost::get<4>(tests[A]);
                break;
            }
        }

        // Build V1 and V2
        double v3[3];
        v3[0] = Vp[0][2];
        v3[1] = Vp[1][2];
        v3[2] = Vp[2][2];

        double v1[3];
        v1[0] = mol->x(index) - Xp[0]; 
        v1[1] = mol->y(index) - Xp[1]; 
        v1[2] = mol->z(index) - Xp[2]; 

        double zproj = v1[0] * v3[0] +     
                       v1[1] * v3[1] +     
                       v1[2] * v3[2];     

        v1[0] -= zproj * v3[0];
        v1[1] -= zproj * v3[1];
        v1[2] -= zproj * v3[2];

        double v1norm = sqrt(v1[0] * v1[0] + v1[1] * v1[1] + v1[2] * v1[2]);
        v1[0] /= v1norm;
        v1[1] /= v1norm;
        v1[2] /= v1norm;

        double v2[3];
        v2[0] = (v1[1] * v3[2] - v1[2] * v3[1]); 
        v2[1] = -(v1[0] * v3[2] - v1[2] * v3[0]); 
        v2[2] = (v1[0] * v3[1] - v1[1] * v3[0]); 

        double v2norm = sqrt(v2[0] * v2[0] + v2[1] * v2[1] + v2[2] * v2[2]);
        v2[0] /= v2norm;
        v2[1] /= v2norm;
        v2[2] /= v2norm;

        Vp[0][0] = v1[0];
        Vp[1][0] = v1[1];
        Vp[2][0] = v1[2];

        Vp[0][1] = v2[0];
        Vp[1][1] = v2[1];
        Vp[2][1] = v2[2];

        R->copy(V);

    } else {
        // Asymmetric top
        fprintf(outfile, "    Asymmetric top\n\n");
        R->copy(V);
    }

    R->print();

    return R;
}
boost::shared_ptr<GridBlock> MolecularGrid::fullGrid() 
{
    boost::shared_ptr<GridBlock>g(new GridBlock());
    g->setMaxPoints(npoints_);
    g->setTruePoints(npoints_);
    g->setGrid(x_,y_,z_,w_);
    return g;
}
void MolecularGrid::buildGrid(std::vector<boost::shared_ptr<AtomicGrid> >& atoms, const std::string& scheme,
   boost::shared_ptr<BasisExtents> extents, int max_points, int min_points)
{
    extents_ = extents;
    primary_ = extents_->basis();
    atoms_ = atoms;
        
    if (npoints_) {
        delete[] x_;
        delete[] y_;
        delete[] z_;
        delete[] w_;
    }

    npoints_ = 0;
    for (int i = 0; i < atoms_.size(); i++) {
        npoints_ += atoms_[i]->npoints();
    }

    x_ = new double[npoints_];
    y_ = new double[npoints_];
    z_ = new double[npoints_];
    w_ = new double[npoints_];
   
    for (int i = 0, ij = 0; i < atoms_.size(); i++) {   
        double* x = atoms_[i]->x();
        double* y = atoms_[i]->y();
        double* z = atoms_[i]->z();
        double* w = atoms_[i]->w();
        for (int j = 0; j < atoms_[i]->npoints(); j++, ij++) {
            x_[ij] = x[j];
            y_[ij] = y[j];
            z_[ij] = z[j];
            w_[ij] = w[j];
        }
    }

    if (boost::to_upper_copy(scheme) == "NAIVE") {
        applyNaiveWeights();
    } else if (boost::to_upper_copy(scheme) == "BECKE") {
        applyBeckeWeights();
    } else if (boost::to_upper_copy(scheme) == "TREUTLER") {
        applyTreutlerWeights();
    } else if (boost::to_upper_copy(scheme) == "STRATMANN") {
        applyStratmannWeights();
    } else {
        throw PSIEXCEPTION("Invalid Nuclear weighting scheme");
    }

    sieve();
    block(max_points, min_points);
}
void MolecularGrid::getBSRadii()
{
    if (BSRadii_.size() == 0) {
        BSRadii_.push_back(1.000);     
        BSRadii_.push_back(1.001);     
        BSRadii_.push_back(1.012);     
        BSRadii_.push_back(0.825);     
        BSRadii_.push_back(1.408);     
        BSRadii_.push_back(1.485);     
        BSRadii_.push_back(1.452);     
        BSRadii_.push_back(1.397);     
        BSRadii_.push_back(1.342);     
        BSRadii_.push_back(1.287);     
        BSRadii_.push_back(1.243);     
        BSRadii_.push_back(1.144);     
        BSRadii_.push_back(1.364);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.716);     
        BSRadii_.push_back(1.705);     
        BSRadii_.push_back(1.683);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.595);     
        BSRadii_.push_back(1.485);     
        BSRadii_.push_back(1.474);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.562);     
        BSRadii_.push_back(1.650);     
        BSRadii_.push_back(1.727);     
        BSRadii_.push_back(1.760);     
        BSRadii_.push_back(1.771);     
        BSRadii_.push_back(1.749);     
        BSRadii_.push_back(1.727);     
        BSRadii_.push_back(1.628);     
        BSRadii_.push_back(1.606);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.639);     
        BSRadii_.push_back(1.672);     
        BSRadii_.push_back(1.804);     
        BSRadii_.push_back(1.881);     
        BSRadii_.push_back(1.892);     
        BSRadii_.push_back(1.892);     
        BSRadii_.push_back(1.881);     
    }
}
void MolecularGrid::applyStandardWeights(SharedMatrix chi_values)
{
    double** chip = chi_values->pointer();
    int natom = molecule_->natom();

    for (int i = 0; i< natom; i++) {
        for (int j = 0; j< natom; j++) {
            if (i == j) continue;
            chip[i][j] = (chip[i][j] - 1.0) / (chip[i][j] + 1.0);
            chip[i][j] /= (chip[i][j] * chip[i][j] - 1.0);
            chip[i][j] = (chip[i][j] < -0.5 ? -0.5 : chip[i][j]);
            chip[i][j] = (chip[i][j] > 0.5 ? 0.5 : chip[i][j]);
        }
    }

    // Now chip is the aij value
    double** inv_dist = block_matrix(natom, natom); 

    // Build the inverse distance map
    for (int A = 0; A < natom; A++) {
        for (int B = 0; B < natom; B++) {
            if (A == B) continue;
            inv_dist[A][B] = 1.0 / sqrt((molecule_->x(A) - molecule_->x(B)) * (molecule_->x(A) - molecule_->x(B)) +    
                           (molecule_->y(A) - molecule_->y(B)) * (molecule_->y(A) - molecule_->y(B)) +    
                           (molecule_->z(A) - molecule_->z(B)) * (molecule_->z(A) - molecule_->z(B)));   
        } 
    }

    double* dist = new double[natom];

    for (int A = 0, ij = 0; A < natom; A++) {
        for (int k = 0; k < atoms_[A]->npoints(); k++, ij++) {
            double x = x_[ij]; 
            double y = y_[ij]; 
            double z = z_[ij]; 
            
            for (int l = 0; l < natom; l++) { 
                dist[l] = sqrt((x - molecule_->x(l)) * (x - molecule_->x(l)) + 
                               (y - molecule_->y(l)) * (y - molecule_->y(l)) + 
                               (z - molecule_->z(l)) * (z - molecule_->z(l))); 
            }
            
            double numerator = 0.0; 
            double denominator = 0.0;
            double prod, mu, nu, s;
            for (int i =0; i <natom; i++) {
                prod = 1.0;
                for (int j = 0; j < natom; j++) {
                    if (i == j)
                        continue;
                    mu = (dist[i] - dist[j])*inv_dist[i][j];
                    nu = mu + chip[i][j]*(1.0-mu*mu);
                    s = nu;
                    s = 1.5*s - 0.5*s*s*s;
                    s = 1.5*s - 0.5*s*s*s;
                    s = 1.5*s - 0.5*s*s*s;
                    s = 0.5*(1.0-s);
                    prod *= s;
                }
                if (i == A) numerator = prod;
                denominator += prod;
            }
            w_[ij] *= numerator / denominator;
        }
    }

    delete[] dist;
    free_block(inv_dist);
} 
void MolecularGrid::applyNaiveWeights()
{
    scheme_ = "Naive"; 

    int natom = molecule_->natom();

    SharedMatrix chi(new Matrix("Chi values", natom, natom));
    double** chip = chi->pointer();

    MolecularGrid::getBSRadii();

    for (int i = 0; i < natom; i++) {
        for (int j = 0; j < i; j++) {
            chip[i][j] = 1.0;
            chip[j][i] = 1.0;
        }
    }
    
    applyStandardWeights(chi);
}
void MolecularGrid::applyBeckeWeights()
{
    scheme_ = "Naive"; 

    int natom = molecule_->natom();

    SharedMatrix chi(new Matrix("Chi values", natom, natom));
    double** chip = chi->pointer();

    MolecularGrid::getBSRadii();

    for (int i = 0; i < natom; i++) {
        for (int j = 0; j < i; j++) {
            chip[i][j] = BSRadii_[molecule_->Z(i)] / BSRadii_[molecule_->Z(j)];
            chip[j][i] = 1.0 / chip[i][j];
        }
    }
    
    applyStandardWeights(chi);
}
void MolecularGrid::applyTreutlerWeights()
{
    scheme_ = "Treutler"; 

    int natom = molecule_->natom();

    SharedMatrix chi(new Matrix("Chi values", natom, natom));
    double** chip = chi->pointer();

    MolecularGrid::getBSRadii();

    for (int i = 0; i < natom; i++) {
        for (int j = 0; j < i; j++) {
            chip[i][j] = sqrt(BSRadii_[molecule_->Z(i)] / BSRadii_[molecule_->Z(j)]);
            chip[j][i] = 1.0 / chip[i][j];
        }
    }
    
    applyStandardWeights(chi);
}
void MolecularGrid::applyStratmannWeights()
{
    throw FeatureNotImplemented("psi::MolecularGrid","applyStratmannWeights", __FILE__, __LINE__);
}
void MolecularGrid::print(FILE* out, int print)
{
    fprintf(out, "   => Molecular Quadrature: %s Scheme <=\n\n", scheme_.c_str());
    fprintf(out, "      Points: %d\n\n", npoints_);
   
    if (print > 4) {
        for (int A = 0; A < atoms_.size(); A++) {
            fprintf(outfile,"\tAtom %d: %s <%8.5f, %8.5f, %8.5f>, %6d Points\n", A+1, molecule_->label(A).c_str(),
                molecule_->x(A), molecule_->y(A), molecule_->z(A), atoms_[A]->npoints());
            boost::shared_ptr<AtomicGrid> atom = atoms_[A];
            boost::shared_ptr<RadialGrid> rad = atom->radial();
            std::vector<boost::shared_ptr<SphericalGrid> > sphere = atom->spherical();
            fprintf(outfile,"\t Radial Grid: %s, %4d Points\n", rad->scheme().c_str(), rad->npoints());
            for (int i = 0; i < sphere.size(); i++) {
                fprintf(outfile,"\t  Spherical Grid: R %11.5E, Order %4d, Points %4d, %s\n", 
                    rad->r()[i], sphere[i]->order(), sphere[i]->npoints(),sphere[i]->scheme().c_str());    
            }   
            fprintf(outfile, "\n"); 
        }
    }
 
    if (print > 5) {

        for (int i = 0; i < atoms_.size(); i++) {
            fprintf(out, "   Atomic Quadrature %d:\n\n", i+1);
            atoms_[i]->print(out);
        }

        fprintf(out, "   Total Quadrature:\n\n");
        fprintf(out, "   %4s %14s %14s %14s %14s\n", "N", "X", "Y", "Z", "W");
        for (int i = 0; i < npoints_; i++) {
            fprintf(out, "   %4d %14.6E %14.6E %14.6E %14.6E\n", i+1, 
                x_[i], y_[i], z_[i], w_[i]); 
        } 
        fprintf(out, "\n\n");
    }

    if (print > 2) {
        extents_->print();
    }
}

AtomicGrid::AtomicGrid() : npoints_(0)
{
}
AtomicGrid::~AtomicGrid() 
{
    if (npoints_) {
        delete[] x_;
        delete[] y_;
        delete[] z_;
        delete[] w_;
    }
}
void AtomicGrid::buildGrid(const Vector3& center,
                      SharedMatrix rotation,
                      boost::shared_ptr<RadialGrid> radial,
                      std::vector<boost::shared_ptr<SphericalGrid> > sphericals)  
{
    center_ = center;
    rotation_ = rotation;
    radial_ = radial;
    sphericals_ = sphericals;

    if (npoints_) {
        delete[] x_;
        delete[] y_;
        delete[] z_;
        delete[] w_;
    }

    npoints_ = 0;
    for (int i = 0; i < sphericals_.size(); i++) {
        npoints_ += sphericals_[i]->npoints();
    }    

    x_ = new double[npoints_];
    y_ = new double[npoints_];
    z_ = new double[npoints_];
    w_ = new double[npoints_];

    double* r = radial_->r();
    double* wr = radial_->w_r();

    // Direct product
    int counter = 0;
    for (int i = 0, ij = 0; i < sphericals_.size(); i++) {

        double Rr = r[i];
        double Wr = wr[i];    

        double* x = sphericals_[i]->x();
        double* y = sphericals_[i]->y();
        double* z = sphericals_[i]->z();
        double* wth = sphericals_[i]->w();
        for (int j = 0; j < sphericals_[i]->npoints(); j++, ij++) {
            x_[ij] = Rr * x[j]; 
            y_[ij] = Rr * y[j]; 
            z_[ij] = Rr * z[j]; 
            w_[ij] = Wr * wth[j]; 
        }
    }

    // Rotate
    double** Rot = rotation_->pointer();
    double R00 = Rot[0][0];
    double R01 = Rot[0][1];
    double R02 = Rot[0][2];
    double R10 = Rot[1][0];
    double R11 = Rot[1][1];
    double R12 = Rot[1][2];
    double R20 = Rot[2][0];
    double R21 = Rot[2][1];
    double R22 = Rot[2][2];
    double xh, yh, zh;
    for (int ij = 0; ij < npoints_; ij++) {
        xh = x_[ij]; 
        yh = y_[ij]; 
        zh = z_[ij]; 
        x_[ij] = R00 * xh + R01 * yh + R02 * zh; 
        y_[ij] = R10 * xh + R11 * yh + R12 * zh; 
        z_[ij] = R20 * xh + R21 * yh + R22 * zh; 
    }

    // Translate
    for (int ij = 0; ij < npoints_; ij++) {
        x_[ij] += center_[0];
        y_[ij] += center_[1];
        z_[ij] += center_[2];
    } 
}
void AtomicGrid::print(FILE* out)
{
    fprintf(out, "   => Atomic Quadrature <=\n\n");
    fprintf(out, "      Points: %d\n\n", npoints_);
    
    fprintf(out, "   Radial Quadrature:\n\n");
    radial_->print(out);

    fprintf(out, "   Total Atomic Quadrature:\n\n");
    for (int i = 0; i < sphericals_.size(); i++) {
        fprintf(out, "   Spherical Quadrature %d:\n\n", i+1);
        sphericals_[i]->print(out);
    }

    fprintf(out, "   %4s %14s %14s %14s %14s\n", "N", "X", "Y", "Z", "W");
    for (int i = 0; i < npoints_; i++) {
        fprintf(out, "   %4d %14.6E %14.6E %14.6E %14.6E\n", i+1, 
            x_[i], y_[i], z_[i], w_[i]); 
    } 
    fprintf(out, "\n\n");
}

RadialGrid::RadialGrid() :
    npoints_(0), scheme_("")
{
}
RadialGrid::~RadialGrid()
{
    if (npoints_) {
        delete[] r_;
        delete[] w_x_;
        delete[] w_r_;
    }
}
void RadialGrid::print(FILE* out) 
{
    fprintf(out, "   => Radial Quadrature: %s Scheme <=\n\n", scheme_.c_str());
    fprintf(out, "      Points: %d\n\n", npoints_);
    fprintf(out, "   %4s %14s %14s %14s\n", "N", "R", "W", "WR^2");
    for (int i = 0; i < npoints_; i++) {
        fprintf(out, "   %4d %14.6E %14.6E %14.6E\n", i+1, r_[i], 
            w_x_[i], w_r_[i]); 
    } 
    fprintf(out, "\n\n");
}
boost::shared_ptr<RadialGrid> RadialGrid::buildGrid(const std::string& scheme, int n, double xi)
{
    if (boost::to_upper_copy(scheme) == "TREUTLER") {
        return RadialGrid::buildTreutlerGrid(n, xi);
    } else if (boost::to_upper_copy(scheme) == "BECKE") {
        return RadialGrid::buildBeckeGrid(n, xi);
    } else if (boost::to_upper_copy(scheme) == "EM") {
        return RadialGrid::buildEMGrid(n, xi);
    } else if (boost::to_upper_copy(scheme) == "MURA") {
        return RadialGrid::buildMuraGrid(n, xi);
    } else if (boost::to_upper_copy(scheme) == "MUTIEXP") {
        return RadialGrid::buildMultiExpGrid(n, xi);
    } else {
        throw PSIEXCEPTION("Unrecognized radial grid type.");
    }
}
boost::shared_ptr<RadialGrid> RadialGrid::buildTreutlerGrid(int n, double xi, double alpha)
{
    RadialGrid* r = new RadialGrid();
    r->scheme_ = "Treutler";
    r->npoints_ = n;

    r->r_ = new double[n];
    r->w_x_ = new double[n];
    r->w_r_ = new double[n];

    double x,temp;
    double INVLN2 = 1.0 / log(2.0);

    for (int tau = 1; tau<=n; tau++) {
        x = cos(tau/(n+1.0)*M_PI);
        r->r_[tau-1] = xi*INVLN2*pow(1.0+x,alpha)*log(2.0/(1.0-x));

        temp = sin(tau/(n+1.0)*M_PI);
        r->w_x_[tau-1] = M_PI/(n+1.0)*temp*temp;
        r->w_x_[tau-1] *= xi*INVLN2*(alpha*pow(1.0+x,alpha-1.0)*log(2.0/(1.0-x))+pow(1.0+x,alpha)/(1.0-x));
        r->w_x_[tau-1] *= 1.0/sqrt(1.0-x*x);
        r->w_r_[tau-1] = r->w_x_[tau-1]*r->r_[tau-1]*r->r_[tau-1];
    }

    return boost::shared_ptr<RadialGrid>(r);
} 
boost::shared_ptr<RadialGrid> RadialGrid::buildBeckeGrid(int n, double xi)
{
    RadialGrid* r = new RadialGrid();
    r->scheme_ = "Becke";
    r->npoints_ = n;

    r->r_ = new double[n];
    r->w_x_ = new double[n];
    r->w_r_ = new double[n];

    double x,temp;

    for (int tau = 1; tau<=n; tau++) {
        x = cos(tau/(n+1.0)*M_PI);
        r->r_[tau-1] = xi*(1.0-x)/(1.0+x);

        temp = sin(tau/(n+1.0)*M_PI);
        r->w_x_[tau-1] = M_PI/(n+1.0)*temp*temp*xi*2.0/((1.0+x)*(1.0+x)*sqrt(1.0-x*x));
        r->w_r_[tau-1] = r->w_x_[tau-1]*r->r_[tau-1]*r->r_[tau-1];
    }

    return boost::shared_ptr<RadialGrid>(r);
} 
boost::shared_ptr<RadialGrid> RadialGrid::buildMuraGrid(int n, double xi)
{
    throw FeatureNotImplemented("psi::RadialGrid","buildMuraGrid", __FILE__, __LINE__);
} 
boost::shared_ptr<RadialGrid> RadialGrid::buildMultiExpGrid(int n, double xi)
{
    throw FeatureNotImplemented("psi::RadialGrid","buildMultiExpGrid", __FILE__, __LINE__);
} 
boost::shared_ptr<RadialGrid> RadialGrid::buildEMGrid(int n, double xi)
{
    throw FeatureNotImplemented("psi::RadialGrid","buildEMGrid", __FILE__, __LINE__);
} 

SphericalGrid::SphericalGrid() :
    npoints_(0), order_(0), scheme_("")
{
}
SphericalGrid::~SphericalGrid()
{
    if (npoints_) {
        delete[] x_;
        delete[] y_;
        delete[] z_;
        delete[] w_;
    }
}
void SphericalGrid::print(FILE* out) 
{
    fprintf(out, "   => Spherical Quadrature: %s Scheme <=\n\n", scheme_.c_str());
    fprintf(out, "      Points: %d, Order %d\n\n", npoints_, order_);
    fprintf(out, "   %4s %14s %14s %14s %14s\n", "N", "X", "Y", "Z", "W");
    for (int i = 0; i < npoints_; i++) {
        fprintf(out, "   %4d %14.6E %14.6E %14.6E %14.6E\n", i+1, 
            x_[i], y_[i], z_[i], w_[i]); 
    } 
    fprintf(out, "\n\n");
}
boost::shared_ptr<SphericalGrid> SphericalGrid::buildGrid(const std::string& scheme, int order)
{
    if (boost::to_upper_copy(scheme) == "LEBEDEV") {
        return SphericalGrid::lookupLebedevGrid(order);
    } else {
        throw PSIEXCEPTION("Unrecognized spherical grid type.");
    }
}
boost::shared_ptr<SphericalGrid> SphericalGrid::lookupLebedevGrid(int order) 
{
    if (lebedev_grids_.size() == 0) {
        lebedev_grids_[0]   = buildLebedevGrid(0);   
        lebedev_grids_[3]   = buildLebedevGrid(3); 
        lebedev_grids_[5]   = buildLebedevGrid(5); 
        lebedev_grids_[7]   = buildLebedevGrid(7); 
        lebedev_grids_[9]   = buildLebedevGrid(9); 
        lebedev_grids_[11]  = buildLebedevGrid(11); 
        lebedev_grids_[13]  = buildLebedevGrid(13); 
        lebedev_grids_[15]  = buildLebedevGrid(15); 
        lebedev_grids_[17]  = buildLebedevGrid(17); 
        lebedev_grids_[19]  = buildLebedevGrid(19); 
        lebedev_grids_[21]  = buildLebedevGrid(21); 
        lebedev_grids_[23]  = buildLebedevGrid(23); 
        lebedev_grids_[25]  = buildLebedevGrid(25); 
        lebedev_grids_[27]  = buildLebedevGrid(27); 
        lebedev_grids_[29]  = buildLebedevGrid(29); 
        lebedev_grids_[31]  = buildLebedevGrid(31); 
        lebedev_grids_[35]  = buildLebedevGrid(35); 
        lebedev_grids_[41]  = buildLebedevGrid(41); 
        lebedev_grids_[47]  = buildLebedevGrid(47); 
        lebedev_grids_[53]  = buildLebedevGrid(53); 
        lebedev_grids_[59]  = buildLebedevGrid(59); 
        lebedev_grids_[65]  = buildLebedevGrid(65); 
        lebedev_grids_[71]  = buildLebedevGrid(71); 
        lebedev_grids_[77]  = buildLebedevGrid(77); 
        lebedev_grids_[83]  = buildLebedevGrid(83); 
        lebedev_grids_[89]  = buildLebedevGrid(89); 
        lebedev_grids_[95]  = buildLebedevGrid(95); 
        lebedev_grids_[101] = buildLebedevGrid(101); 
        lebedev_grids_[107] = buildLebedevGrid(107); 
        lebedev_grids_[113] = buildLebedevGrid(113); 
        lebedev_grids_[119] = buildLebedevGrid(119); 
        lebedev_grids_[125] = buildLebedevGrid(125); 
        lebedev_grids_[131] = buildLebedevGrid(131); 
    }
    if (lebedev_grids_.count(order) == 0) {
        fprintf(outfile, " Valid Lebedev Grid Orders/Points:\n\n");
    
        // Sort keys for pretty printing
        std::vector<int> keys;
        for (std::map<int,int>::iterator it = lebedev_order_to_points_.begin();
             it != lebedev_order_to_points_.end(); it++) {
            keys.push_back((*it).first);  
        }    
        std::sort(keys.begin(), keys.end());
        
        for (int i = 0; i < keys.size(); i++) {
            fprintf(outfile, "  %4d - %4d\n", keys[i], lebedev_order_to_points_[keys[i]]); 
        }

        fflush(outfile);
        throw PSIEXCEPTION("Invalid Lebedev Grid requested");
    }

    return lebedev_grids_[order];
}
std::map<int,int> SphericalGrid::lebedevOrdersToPoints()
{
    // Setup the valid grid order/points map
    if (lebedev_order_to_points_.size() == 0) {
        lebedev_order_to_points_[0]   = 1;
        lebedev_order_to_points_[3]   = 6;
        lebedev_order_to_points_[5]   = 14;
        lebedev_order_to_points_[7]   = 26;
        lebedev_order_to_points_[9]   = 38;
        lebedev_order_to_points_[11]  = 50;
        lebedev_order_to_points_[13]  = 74;
        lebedev_order_to_points_[15]  = 86;
        lebedev_order_to_points_[17]  = 110;
        lebedev_order_to_points_[19]  = 146;
        lebedev_order_to_points_[21]  = 170;
        lebedev_order_to_points_[23]  = 194;
        lebedev_order_to_points_[25]  = 230;
        lebedev_order_to_points_[27]  = 266;
        lebedev_order_to_points_[29]  = 302;
        lebedev_order_to_points_[31]  = 350;
        lebedev_order_to_points_[35]  = 434;
        lebedev_order_to_points_[41]  = 590;
        lebedev_order_to_points_[47]  = 770;
        lebedev_order_to_points_[53]  = 974;
        lebedev_order_to_points_[59]  = 1202;
        lebedev_order_to_points_[65]  = 1454;
        lebedev_order_to_points_[71]  = 1730;
        lebedev_order_to_points_[77]  = 2030;
        lebedev_order_to_points_[83]  = 2354;
        lebedev_order_to_points_[89]  = 2702;
        lebedev_order_to_points_[95]  = 3074;
        lebedev_order_to_points_[101] = 3470;
        lebedev_order_to_points_[107] = 3890;
        lebedev_order_to_points_[113] = 4334;
        lebedev_order_to_points_[119] = 4802;
        lebedev_order_to_points_[125] = 5294;
        lebedev_order_to_points_[131] = 5810;
    }
    return lebedev_order_to_points_;
}
boost::shared_ptr<SphericalGrid> SphericalGrid::buildLebedevGrid(int order)
{
    lebedevOrdersToPoints();

    // We don't have your grid
    if (lebedev_order_to_points_.count(order) == 0) {
        fprintf(outfile, " Valid Lebedev Grid Orders/Points:\n\n");
    
        // Sort keys for pretty printing
        std::vector<int> keys;
        for (std::map<int,int>::iterator it = lebedev_order_to_points_.begin();
             it != lebedev_order_to_points_.end(); it++) {
            keys.push_back((*it).first);  
        }    
        std::sort(keys.begin(), keys.end());
        
        for (int i = 0; i < keys.size(); i++) {
            fprintf(outfile, "  %4d - %4d\n", keys[i], lebedev_order_to_points_[keys[i]]); 
        }

        fflush(outfile);
        throw PSIEXCEPTION("Invalid Lebedev Grid requested");
    }

    int npoints = lebedev_order_to_points_[order];

    SphericalGrid* s = new SphericalGrid();
    s->scheme_ = "Lebedev-Laikov";
    s->order_ = order;
    s->npoints_ = npoints; 
    
    s->x_ = new double[npoints]; 
    s->y_ = new double[npoints]; 
    s->z_ = new double[npoints]; 
    s->w_ = new double[npoints]; 
   
    ::memset(static_cast<void*>(s->x_), '\0', npoints*sizeof(double));
    ::memset(static_cast<void*>(s->y_), '\0', npoints*sizeof(double));
    ::memset(static_cast<void*>(s->z_), '\0', npoints*sizeof(double));
    ::memset(static_cast<void*>(s->w_), '\0', npoints*sizeof(double));
 
    //Get Lebedev sphere of number of points degree
    //Translated from FORTRAN code
    //This one requires a bit of faith
    //The Soviets did their job
    int start=0;
    double a = 0.0;
    double b = 0.0;
    double v = 0.0;

    switch (npoints){

    case 1:
        s->x_[0] = 0.0;
        s->y_[0] = 0.0;
        s->z_[0] = 1.0;
        s->w_[0] = 4*M_PI;
        break;

    case 6:

    v=0.1666666666666667E+0;
    start = lebedevReccurence(1,start,a,b,v,s);
    break;

    case 14:

    v=0.6666666666666667E-1;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.7500000000000000E-1;
    start = lebedevReccurence(3,start,a,b,v,s);
    break;

    case 26:

    v=0.4761904761904762E-1;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.3809523809523810E-1;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.3214285714285714E-1;
    start = lebedevReccurence(3,start,a,b,v,s);
    break;

    case 38:

    v=0.9523809523809524E-2;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.3214285714285714E-1;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.4597008433809831E+0;
    v=0.2857142857142857E-1;
    start = lebedevReccurence(5,start,a,b,v,s);
    break;

    case 50:

    v=0.1269841269841270E-1;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.2257495590828924E-1;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.2109375000000000E-1;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.3015113445777636E+0;
    v=0.2017333553791887E-1;
    start = lebedevReccurence(4,start,a,b,v,s);
    break;

    case 74:

    v=0.5130671797338464E-3;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.1660406956574204E-1;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=-0.2958603896103896E-1;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.4803844614152614E+0;
    v=0.2657620708215946E-1;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3207726489807764E+0;
    v=0.1652217099371571E-1;
    start = lebedevReccurence(5,start,a,b,v,s);
    break;

    case 86:

    v=0.1154401154401154E-1;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.1194390908585628E-1;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.3696028464541502E+0;
    v=0.1111055571060340E-1;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6943540066026664E+0;
    v=0.1187650129453714E-1;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3742430390903412E+0;
    v=0.1181230374690448E-1;
    start = lebedevReccurence(5,start,a,b,v,s);
    break;

    case 110:

    v=0.3828270494937162E-2;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.9793737512487512E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.1851156353447362E+0;
    v=0.8211737283191111E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6904210483822922E+0;
    v=0.9942814891178103E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3956894730559419E+0;
    v=0.9595471336070963E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4783690288121502E+0;
    v=0.9694996361663028E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    break;

    case 146:

    v=0.5996313688621381E-3;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.7372999718620756E-2;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.7210515360144488E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.6764410400114264E+0;
    v=0.7116355493117555E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4174961227965453E+0;
    v=0.6753829486314477E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1574676672039082E+0;
    v=0.7574394159054034E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1403553811713183E+0;
    b=0.4493328323269557E+0;
    v=0.6991087353303262E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 170:

    v=0.5544842902037365E-2;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.6071332770670752E-2;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.6383674773515093E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.2551252621114134E+0;
    v=0.5183387587747790E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6743601460362766E+0;
    v=0.6317929009813725E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4318910696719410E+0;
    v=0.6201670006589077E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2613931360335988E+0;
    v=0.5477143385137348E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4990453161796037E+0;
    b=0.1446630744325115E+0;
    v=0.5968383987681156E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 194:

    v=0.1782340447244611E-2;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.5716905949977102E-2;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.5573383178848738E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.6712973442695226E+0;
    v=0.5608704082587997E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2892465627575439E+0;
    v=0.5158237711805383E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4446933178717437E+0;
    v=0.5518771467273614E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1299335447650067E+0;
    v=0.4106777028169394E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3457702197611283E+0;
    v=0.5051846064614808E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1590417105383530E+0;
    b=0.8360360154824589E+0;
    v=0.5530248916233094E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 230:

    v=-0.5522639919727325E-1;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.4450274607445226E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.4492044687397611E+0;
    v=0.4496841067921404E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2520419490210201E+0;
    v=0.5049153450478750E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6981906658447242E+0;
    v=0.3976408018051883E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6587405243460960E+0;
    v=0.4401400650381014E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4038544050097660E-1;
    v=0.1724544350544401E-1;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5823842309715585E+0;
    v=0.4231083095357343E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3545877390518688E+0;
    v=0.5198069864064399E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2272181808998187E+0;
    b=0.4864661535886647E+0;
    v=0.4695720972568883E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 266:

    v=-0.1313769127326952E-2;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=-0.2522728704859336E-2;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.4186853881700583E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.7039373391585475E+0;
    v=0.5315167977810885E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1012526248572414E+0;
    v=0.4047142377086219E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4647448726420539E+0;
    v=0.4112482394406990E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3277420654971629E+0;
    v=0.3595584899758782E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6620338663699974E+0;
    v=0.4256131351428158E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.8506508083520399E+0;
    v=0.4229582700647240E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3233484542692899E+0;
    b=0.1153112011009701E+0;
    v=0.4080914225780505E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2314790158712601E+0;
    b=0.5244939240922365E+0;
    v=0.4071467593830964E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 302:

    v=0.8545911725128148E-3;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.3599119285025571E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.3515640345570105E+0;
    v=0.3449788424305883E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6566329410219612E+0;
    v=0.3604822601419882E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4729054132581005E+0;
    v=0.3576729661743367E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.9618308522614784E-1;
    v=0.2352101413689164E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2219645236294178E+0;
    v=0.3108953122413675E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7011766416089545E+0;
    v=0.3650045807677255E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2644152887060663E+0;
    v=0.2982344963171804E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5718955891878961E+0;
    v=0.3600820932216460E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2510034751770465E+0;
    b=0.8000727494073952E+0;
    v=0.3571540554273387E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1233548532583327E+0;
    b=0.4127724083168531E+0;
    v=0.3392312205006170E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 350:

    v=0.3006796749453936E-2;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.3050627745650771E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.7068965463912316E+0;
    v=0.1621104600288991E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4794682625712025E+0;
    v=0.3005701484901752E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1927533154878019E+0;
    v=0.2990992529653774E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6930357961327123E+0;
    v=0.2982170644107595E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3608302115520091E+0;
    v=0.2721564237310992E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6498486161496169E+0;
    v=0.3033513795811141E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1932945013230339E+0;
    v=0.3007949555218533E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3800494919899303E+0;
    v=0.2881964603055307E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2899558825499574E+0;
    b=0.7934537856582316E+0;
    v=0.2958357626535696E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.9684121455103957E-1;
    b=0.8280801506686862E+0;
    v=0.3036020026407088E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1833434647041659E+0;
    b=0.9074658265305127E+0;
    v=0.2832187403926303E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 434:

    v=0.5265897968224436E-3;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.2548219972002607E-2;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.2512317418927307E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.6909346307509111E+0;
    v=0.2530403801186355E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1774836054609158E+0;
    v=0.2014279020918528E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4914342637784746E+0;
    v=0.2501725168402936E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6456664707424256E+0;
    v=0.2513267174597564E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2861289010307638E+0;
    v=0.2302694782227416E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7568084367178018E-1;
    v=0.1462495621594614E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3927259763368002E+0;
    v=0.2445373437312980E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.8818132877794288E+0;
    v=0.2417442375638981E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.9776428111182649E+0;
    v=0.1910951282179532E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2054823696403044E+0;
    b=0.8689460322872412E+0;
    v=0.2416930044324775E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5905157048925271E+0;
    b=0.7999278543857286E+0;
    v=0.2512236854563495E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5550152361076807E+0;
    b=0.7717462626915901E+0;
    v=0.2496644054553086E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.9371809858553722E+0;
    b=0.3344363145343455E+0;
    v=0.2236607760437849E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 590:

    v=0.3095121295306187E-3;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.1852379698597489E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.7040954938227469E+0;
    v=0.1871790639277744E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6807744066455243E+0;
    v=0.1858812585438317E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6372546939258752E+0;
    v=0.1852028828296213E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5044419707800358E+0;
    v=0.1846715956151242E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4215761784010967E+0;
    v=0.1818471778162769E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3317920736472123E+0;
    v=0.1749564657281154E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2384736701421887E+0;
    v=0.1617210647254411E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1459036449157763E+0;
    v=0.1384737234851692E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6095034115507196E-1;
    v=0.9764331165051050E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6116843442009876E+0;
    v=0.1857161196774078E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3964755348199858E+0;
    v=0.1705153996395864E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1724782009907724E+0;
    v=0.1300321685886048E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5610263808622060E+0;
    b=0.3518280927733519E+0;
    v=0.1842866472905286E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4742392842551980E+0;
    b=0.2634716655937950E+0;
    v=0.1802658934377451E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5984126497885380E+0;
    b=0.1816640840360209E+0;
    v=0.1849830560443660E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3791035407695563E+0;
    b=0.1720795225656878E+0;
    v=0.1713904507106709E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2778673190586244E+0;
    b=0.8213021581932511E-1;
    v=0.1555213603396808E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5033564271075117E+0;
    b=0.8999205842074875E-1;
    v=0.1802239128008525E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 770:

    v=0.2192942088181184E-3;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.1436433617319080E-2;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.1421940344335877E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.5087204410502360E-1;
    v=0.6798123511050502E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1228198790178831E+0;
    v=0.9913184235294912E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2026890814408786E+0;
    v=0.1180207833238949E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2847745156464294E+0;
    v=0.1296599602080921E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3656719078978026E+0;
    v=0.1365871427428316E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4428264886713469E+0;
    v=0.1402988604775325E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5140619627249735E+0;
    v=0.1418645563595609E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6306401219166803E+0;
    v=0.1421376741851662E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6716883332022612E+0;
    v=0.1423996475490962E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6979792685336881E+0;
    v=0.1431554042178567E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1446865674195309E+0;
    v=0.9254401499865368E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3390263475411216E+0;
    v=0.1250239995053509E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5335804651263506E+0;
    v=0.1394365843329230E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6944024393349413E-1;
    b=0.2355187894242326E+0;
    v=0.1127089094671749E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2269004109529460E+0;
    b=0.4102182474045730E+0;
    v=0.1345753760910670E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.8025574607775339E-1;
    b=0.6214302417481605E+0;
    v=0.1424957283316783E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1467999527896572E+0;
    b=0.3245284345717394E+0;
    v=0.1261523341237750E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1571507769824727E+0;
    b=0.5224482189696630E+0;
    v=0.1392547106052696E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2365702993157246E+0;
    b=0.6017546634089558E+0;
    v=0.1418761677877656E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.7714815866765732E-1;
    b=0.4346575516141163E+0;
    v=0.1338366684479554E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3062936666210730E+0;
    b=0.4908826589037616E+0;
    v=0.1393700862676131E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3822477379524787E+0;
    b=0.5648768149099500E+0;
    v=0.1415914757466932E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 974:

    v=0.1438294190527431E-3;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.1125772288287004E-2;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.4292963545341347E-1;
    v=0.4948029341949241E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1051426854086404E+0;
    v=0.7357990109125470E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1750024867623087E+0;
    v=0.8889132771304384E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2477653379650257E+0;
    v=0.9888347838921435E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3206567123955957E+0;
    v=0.1053299681709471E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3916520749849983E+0;
    v=0.1092778807014578E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4590825874187624E+0;
    v=0.1114389394063227E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5214563888415861E+0;
    v=0.1123724788051555E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6253170244654199E+0;
    v=0.1125239325243814E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6637926744523170E+0;
    v=0.1126153271815905E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6910410398498301E+0;
    v=0.1130286931123841E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7052907007457760E+0;
    v=0.1134986534363955E-2;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1236686762657990E+0;
    v=0.6823367927109931E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2940777114468387E+0;
    v=0.9454158160447096E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4697753849207649E+0;
    v=0.1074429975385679E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6334563241139567E+0;
    v=0.1129300086569132E-2;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5974048614181342E-1;
    b=0.2029128752777523E+0;
    v=0.8436884500901954E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1375760408473636E+0;
    b=0.4602621942484054E+0;
    v=0.1075255720448885E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3391016526336286E+0;
    b=0.5030673999662036E+0;
    v=0.1108577236864462E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1271675191439820E+0;
    b=0.2817606422442134E+0;
    v=0.9566475323783357E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2693120740413512E+0;
    b=0.4331561291720157E+0;
    v=0.1080663250717391E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1419786452601918E+0;
    b=0.6256167358580814E+0;
    v=0.1126797131196295E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6709284600738255E-1;
    b=0.3798395216859157E+0;
    v=0.1022568715358061E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.7057738183256172E-1;
    b=0.5517505421423520E+0;
    v=0.1108960267713108E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2783888477882155E+0;
    b=0.6029619156159187E+0;
    v=0.1122790653435766E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1979578938917407E+0;
    b=0.3589606329589096E+0;
    v=0.1032401847117460E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2087307061103274E+0;
    b=0.5348666438135476E+0;
    v=0.1107249382283854E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4055122137872836E+0;
    b=0.5674997546074373E+0;
    v=0.1121780048519972E-2;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 1202:

    v=0.1105189233267572E-3;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.9205232738090741E-3;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.9133159786443561E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.3712636449657089E-1;
    v=0.3690421898017899E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.9140060412262223E-1;
    v=0.5603990928680660E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1531077852469906E+0;
    v=0.6865297629282609E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2180928891660612E+0;
    v=0.7720338551145630E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2839874532200175E+0;
    v=0.8301545958894795E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3491177600963764E+0;
    v=0.8686692550179628E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4121431461444309E+0;
    v=0.8927076285846890E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4718993627149127E+0;
    v=0.9060820238568219E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5273145452842337E+0;
    v=0.9119777254940867E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6209475332444019E+0;
    v=0.9128720138604181E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6569722711857291E+0;
    v=0.9130714935691735E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6841788309070143E+0;
    v=0.9152873784554116E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7012604330123631E+0;
    v=0.9187436274321654E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1072382215478166E+0;
    v=0.5176977312965694E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2582068959496968E+0;
    v=0.7331143682101417E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4172752955306717E+0;
    v=0.8463232836379928E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5700366911792503E+0;
    v=0.9031122694253992E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.9827986018263947E+0;
    b=0.1771774022615325E+0;
    v=0.6485778453163257E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.9624249230326228E+0;
    b=0.2475716463426288E+0;
    v=0.7435030910982369E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.9402007994128811E+0;
    b=0.3354616289066489E+0;
    v=0.7998527891839054E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.9320822040143202E+0;
    b=0.3173615246611977E+0;
    v=0.8101731497468018E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.9043674199393299E+0;
    b=0.4090268427085357E+0;
    v=0.8483389574594331E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.8912407560074747E+0;
    b=0.3854291150669224E+0;
    v=0.8556299257311812E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.8676435628462708E+0;
    b=0.4932221184851285E+0;
    v=0.8803208679738260E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.8581979986041619E+0;
    b=0.4785320675922435E+0;
    v=0.8811048182425720E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.8396753624049856E+0;
    b=0.4507422593157064E+0;
    v=0.8850282341265444E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.8165288564022188E+0;
    b=0.5632123020762100E+0;
    v=0.9021342299040653E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.8015469370783529E+0;
    b=0.5434303569693900E+0;
    v=0.9010091677105086E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.7773563069070351E+0;
    b=0.5123518486419871E+0;
    v=0.9022692938426915E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.7661621213900394E+0;
    b=0.6394279634749102E+0;
    v=0.9158016174693465E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.7553584143533510E+0;
    b=0.6269805509024392E+0;
    v=0.9131578003189435E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.7344305757559503E+0;
    b=0.6031161693096310E+0;
    v=0.9107813579482705E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.7043837184021765E+0;
    b=0.5693702498468441E+0;
    v=0.9105760258970126E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 1454:

    v=0.7777160743261247E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.7557646413004701E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.3229290663413854E-1;
    v=0.2841633806090617E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.8036733271462222E-1;
    v=0.4374419127053555E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1354289960531653E+0;
    v=0.5417174740872172E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1938963861114426E+0;
    v=0.6148000891358593E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2537343715011275E+0;
    v=0.6664394485800705E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3135251434752570E+0;
    v=0.7025039356923220E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3721558339375338E+0;
    v=0.7268511789249627E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4286809575195696E+0;
    v=0.7422637534208629E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4822510128282994E+0;
    v=0.7509545035841214E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5320679333566263E+0;
    v=0.7548535057718401E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6172998195394274E+0;
    v=0.7554088969774001E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6510679849127481E+0;
    v=0.7553147174442808E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6777315251687360E+0;
    v=0.7564767653292297E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6963109410648741E+0;
    v=0.7587991808518730E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7058935009831749E+0;
    v=0.7608261832033027E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.9955546194091857E+0;
    v=0.4021680447874916E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.9734115901794209E+0;
    v=0.5804871793945964E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.9275693732388626E+0;
    v=0.6792151955945159E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.8568022422795103E+0;
    v=0.7336741211286294E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.7623495553719372E+0;
    v=0.7581866300989608E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5707522908892223E+0;
    b=0.4387028039889501E+0;
    v=0.7538257859800743E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5196463388403083E+0;
    b=0.3858908414762617E+0;
    v=0.7483517247053123E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4646337531215351E+0;
    b=0.3301937372343854E+0;
    v=0.7371763661112059E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4063901697557691E+0;
    b=0.2725423573563777E+0;
    v=0.7183448895756934E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3456329466643087E+0;
    b=0.2139510237495250E+0;
    v=0.6895815529822191E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2831395121050332E+0;
    b=0.1555922309786647E+0;
    v=0.6480105801792886E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2197682022925330E+0;
    b=0.9892878979686097E-1;
    v=0.5897558896594636E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1564696098650355E+0;
    b=0.4598642910675510E-1;
    v=0.5095708849247346E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6027356673721295E+0;
    b=0.3376625140173426E+0;
    v=0.7536906428909755E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5496032320255096E+0;
    b=0.2822301309727988E+0;
    v=0.7472505965575118E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4921707755234567E+0;
    b=0.2248632342592540E+0;
    v=0.7343017132279698E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4309422998598483E+0;
    b=0.1666224723456479E+0;
    v=0.7130871582177445E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3664108182313672E+0;
    b=0.1086964901822169E+0;
    v=0.6817022032112776E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2990189057758436E+0;
    b=0.5251989784120085E-1;
    v=0.6380941145604121E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6268724013144998E+0;
    b=0.2297523657550023E+0;
    v=0.7550381377920310E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5707324144834607E+0;
    b=0.1723080607093800E+0;
    v=0.7478646640144802E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5096360901960365E+0;
    b=0.1140238465390513E+0;
    v=0.7335918720601220E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4438729938312456E+0;
    b=0.5611522095882537E-1;
    v=0.7110120527658118E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6419978471082389E+0;
    b=0.1164174423140873E+0;
    v=0.7571363978689501E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5817218061802611E+0;
    b=0.5797589531445219E-1;
    v=0.7489908329079234E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 1730:

    v=0.6309049437420976E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.6398287705571748E-3;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.6357185073530720E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.2860923126194662E-1;
    v=0.2221207162188168E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7142556767711522E-1;
    v=0.3475784022286848E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1209199540995559E+0;
    v=0.4350742443589804E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1738673106594379E+0;
    v=0.4978569136522127E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2284645438467734E+0;
    v=0.5435036221998053E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2834807671701512E+0;
    v=0.5765913388219542E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3379680145467339E+0;
    v=0.6001200359226003E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3911355454819537E+0;
    v=0.6162178172717512E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4422860353001403E+0;
    v=0.6265218152438485E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4907781568726057E+0;
    v=0.6323987160974212E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5360006153211468E+0;
    v=0.6350767851540569E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6142105973596603E+0;
    v=0.6354362775297107E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6459300387977504E+0;
    v=0.6352302462706235E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6718056125089225E+0;
    v=0.6358117881417972E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6910888533186254E+0;
    v=0.6373101590310117E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7030467416823252E+0;
    v=0.6390428961368665E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.8354951166354646E-1;
    v=0.3186913449946576E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2050143009099486E+0;
    v=0.4678028558591711E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3370208290706637E+0;
    v=0.5538829697598626E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4689051484233963E+0;
    v=0.6044475907190476E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5939400424557334E+0;
    v=0.6313575103509012E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1394983311832261E+0;
    b=0.4097581162050343E-1;
    v=0.4078626431855630E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1967999180485014E+0;
    b=0.8851987391293348E-1;
    v=0.4759933057812725E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2546183732548967E+0;
    b=0.1397680182969819E+0;
    v=0.5268151186413440E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3121281074713875E+0;
    b=0.1929452542226526E+0;
    v=0.5643048560507316E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3685981078502492E+0;
    b=0.2467898337061562E+0;
    v=0.5914501076613073E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4233760321547856E+0;
    b=0.3003104124785409E+0;
    v=0.6104561257874195E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4758671236059246E+0;
    b=0.3526684328175033E+0;
    v=0.6230252860707806E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5255178579796463E+0;
    b=0.4031134861145713E+0;
    v=0.6305618761760796E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5718025633734589E+0;
    b=0.4509426448342351E+0;
    v=0.6343092767597889E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2686927772723415E+0;
    b=0.4711322502423248E-1;
    v=0.5176268945737826E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3306006819904809E+0;
    b=0.9784487303942695E-1;
    v=0.5564840313313692E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3904906850594983E+0;
    b=0.1505395810025273E+0;
    v=0.5856426671038980E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4479957951904390E+0;
    b=0.2039728156296050E+0;
    v=0.6066386925777091E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5027076848919780E+0;
    b=0.2571529941121107E+0;
    v=0.6208824962234458E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5542087392260217E+0;
    b=0.3092191375815670E+0;
    v=0.6296314297822907E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6020850887375187E+0;
    b=0.3593807506130276E+0;
    v=0.6340423756791859E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4019851409179594E+0;
    b=0.5063389934378671E-1;
    v=0.5829627677107342E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4635614567449800E+0;
    b=0.1032422269160612E+0;
    v=0.6048693376081110E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5215860931591575E+0;
    b=0.1566322094006254E+0;
    v=0.6202362317732461E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5758202499099271E+0;
    b=0.2098082827491099E+0;
    v=0.6299005328403779E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6259893683876795E+0;
    b=0.2618824114553391E+0;
    v=0.6347722390609353E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5313795124811891E+0;
    b=0.5263245019338556E-1;
    v=0.6203778981238834E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5893317955931995E+0;
    b=0.1061059730982005E+0;
    v=0.6308414671239979E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6426246321215801E+0;
    b=0.1594171564034221E+0;
    v=0.6362706466959498E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6511904367376113E+0;
    b=0.5354789536565540E-1;
    v=0.6375414170333233E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 2030:

    v=0.4656031899197431E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.5421549195295507E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.2540835336814348E-1;
    v=0.1778522133346553E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6399322800504915E-1;
    v=0.2811325405682796E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1088269469804125E+0;
    v=0.3548896312631459E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1570670798818287E+0;
    v=0.4090310897173364E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2071163932282514E+0;
    v=0.4493286134169965E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2578914044450844E+0;
    v=0.4793728447962723E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3085687558169623E+0;
    v=0.5015415319164265E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3584719706267024E+0;
    v=0.5175127372677937E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4070135594428709E+0;
    v=0.5285522262081019E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4536618626222638E+0;
    v=0.5356832703713962E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4979195686463577E+0;
    v=0.5397914736175170E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5393075111126999E+0;
    v=0.5416899441599930E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6115617676843916E+0;
    v=0.5419308476889938E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6414308435160159E+0;
    v=0.5416936902030596E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6664099412721607E+0;
    v=0.5419544338703164E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6859161771214913E+0;
    v=0.5428983656630975E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6993625593503890E+0;
    v=0.5442286500098193E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7062393387719380E+0;
    v=0.5452250345057301E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7479028168349763E-1;
    v=0.2568002497728530E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1848951153969366E+0;
    v=0.3827211700292145E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3059529066581305E+0;
    v=0.4579491561917824E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4285556101021362E+0;
    v=0.5042003969083574E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5468758653496526E+0;
    v=0.5312708889976025E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6565821978343439E+0;
    v=0.5438401790747117E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1253901572367117E+0;
    b=0.3681917226439641E-1;
    v=0.3316041873197344E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1775721510383941E+0;
    b=0.7982487607213301E-1;
    v=0.3899113567153771E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2305693358216114E+0;
    b=0.1264640966592335E+0;
    v=0.4343343327201309E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2836502845992063E+0;
    b=0.1751585683418957E+0;
    v=0.4679415262318919E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3361794746232590E+0;
    b=0.2247995907632670E+0;
    v=0.4930847981631031E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3875979172264824E+0;
    b=0.2745299257422246E+0;
    v=0.5115031867540091E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4374019316999074E+0;
    b=0.3236373482441118E+0;
    v=0.5245217148457367E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4851275843340022E+0;
    b=0.3714967859436741E+0;
    v=0.5332041499895321E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5303391803806868E+0;
    b=0.4175353646321745E+0;
    v=0.5384583126021542E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5726197380596287E+0;
    b=0.4612084406355461E+0;
    v=0.5411067210798852E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2431520732564863E+0;
    b=0.4258040133043952E-1;
    v=0.4259797391468714E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3002096800895869E+0;
    b=0.8869424306722721E-1;
    v=0.4604931368460021E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3558554457457432E+0;
    b=0.1368811706510655E+0;
    v=0.4871814878255202E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4097782537048887E+0;
    b=0.1860739985015033E+0;
    v=0.5072242910074885E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4616337666067458E+0;
    b=0.2354235077395853E+0;
    v=0.5217069845235350E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5110707008417874E+0;
    b=0.2842074921347011E+0;
    v=0.5315785966280310E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5577415286163795E+0;
    b=0.3317784414984102E+0;
    v=0.5376833708758905E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6013060431366950E+0;
    b=0.3775299002040700E+0;
    v=0.5408032092069521E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3661596767261781E+0;
    b=0.4599367887164592E-1;
    v=0.4842744917904866E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4237633153506581E+0;
    b=0.9404893773654421E-1;
    v=0.5048926076188130E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4786328454658452E+0;
    b=0.1431377109091971E+0;
    v=0.5202607980478373E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5305702076789774E+0;
    b=0.1924186388843570E+0;
    v=0.5309932388325743E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5793436224231788E+0;
    b=0.2411590944775190E+0;
    v=0.5377419770895208E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6247069017094747E+0;
    b=0.2886871491583605E+0;
    v=0.5411696331677717E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4874315552535204E+0;
    b=0.4804978774953206E-1;
    v=0.5197996293282420E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5427337322059053E+0;
    b=0.9716857199366665E-1;
    v=0.5311120836622945E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5943493747246700E+0;
    b=0.1465205839795055E+0;
    v=0.5384309319956951E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6421314033564943E+0;
    b=0.1953579449803574E+0;
    v=0.5421859504051886E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6020628374713980E+0;
    b=0.4916375015738108E-1;
    v=0.5390948355046314E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6529222529856881E+0;
    b=0.9861621540127005E-1;
    v=0.5433312705027845E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 2354:

    v=0.3922616270665292E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.4703831750854424E-3;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.4678202801282136E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.2290024646530589E-1;
    v=0.1437832228979900E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5779086652271284E-1;
    v=0.2303572493577644E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.9863103576375984E-1;
    v=0.2933110752447454E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1428155792982185E+0;
    v=0.3402905998359838E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1888978116601463E+0;
    v=0.3759138466870372E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2359091682970210E+0;
    v=0.4030638447899798E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2831228833706171E+0;
    v=0.4236591432242211E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3299495857966693E+0;
    v=0.4390522656946746E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3758840802660796E+0;
    v=0.4502523466626247E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4204751831009480E+0;
    v=0.4580577727783541E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4633068518751051E+0;
    v=0.4631391616615899E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5039849474507313E+0;
    v=0.4660928953698676E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5421265793440747E+0;
    v=0.4674751807936953E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6092660230557310E+0;
    v=0.4676414903932920E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6374654204984869E+0;
    v=0.4674086492347870E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6615136472609892E+0;
    v=0.4674928539483207E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6809487285958127E+0;
    v=0.4680748979686447E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6952980021665196E+0;
    v=0.4690449806389040E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7041245497695400E+0;
    v=0.4699877075860818E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6744033088306065E-1;
    v=0.2099942281069176E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1678684485334166E+0;
    v=0.3172269150712804E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2793559049539613E+0;
    v=0.3832051358546523E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3935264218057639E+0;
    v=0.4252193818146985E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5052629268232558E+0;
    v=0.4513807963755000E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6107905315437531E+0;
    v=0.4657797469114178E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1135081039843524E+0;
    b=0.3331954884662588E-1;
    v=0.2733362800522836E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1612866626099378E+0;
    b=0.7247167465436538E-1;
    v=0.3235485368463559E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2100786550168205E+0;
    b=0.1151539110849745E+0;
    v=0.3624908726013453E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2592282009459942E+0;
    b=0.1599491097143677E+0;
    v=0.3925540070712828E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3081740561320203E+0;
    b=0.2058699956028027E+0;
    v=0.4156129781116235E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3564289781578164E+0;
    b=0.2521624953502911E+0;
    v=0.4330644984623263E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4035587288240703E+0;
    b=0.2982090785797674E+0;
    v=0.4459677725921312E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4491671196373903E+0;
    b=0.3434762087235733E+0;
    v=0.4551593004456795E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4928854782917489E+0;
    b=0.3874831357203437E+0;
    v=0.4613341462749918E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5343646791958988E+0;
    b=0.4297814821746926E+0;
    v=0.4651019618269806E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5732683216530990E+0;
    b=0.4699402260943537E+0;
    v=0.4670249536100625E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2214131583218986E+0;
    b=0.3873602040643895E-1;
    v=0.3549555576441708E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2741796504750071E+0;
    b=0.8089496256902013E-1;
    v=0.3856108245249010E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3259797439149485E+0;
    b=0.1251732177620872E+0;
    v=0.4098622845756882E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3765441148826891E+0;
    b=0.1706260286403185E+0;
    v=0.4286328604268950E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4255773574530558E+0;
    b=0.2165115147300408E+0;
    v=0.4427802198993945E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4727795117058430E+0;
    b=0.2622089812225259E+0;
    v=0.4530473511488561E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5178546895819012E+0;
    b=0.3071721431296201E+0;
    v=0.4600805475703138E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5605141192097460E+0;
    b=0.3508998998801138E+0;
    v=0.4644599059958017E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6004763319352512E+0;
    b=0.3929160876166931E+0;
    v=0.4667274455712508E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3352842634946949E+0;
    b=0.4202563457288019E-1;
    v=0.4069360518020356E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3891971629814670E+0;
    b=0.8614309758870850E-1;
    v=0.4260442819919195E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4409875565542281E+0;
    b=0.1314500879380001E+0;
    v=0.4408678508029063E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4904893058592484E+0;
    b=0.1772189657383859E+0;
    v=0.4518748115548597E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5375056138769549E+0;
    b=0.2228277110050294E+0;
    v=0.4595564875375116E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5818255708669969E+0;
    b=0.2677179935014386E+0;
    v=0.4643988774315846E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6232334858144959E+0;
    b=0.3113675035544165E+0;
    v=0.4668827491646946E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4489485354492058E+0;
    b=0.4409162378368174E-1;
    v=0.4400541823741973E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5015136875933150E+0;
    b=0.8939009917748489E-1;
    v=0.4514512890193797E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5511300550512623E+0;
    b=0.1351806029383365E+0;
    v=0.4596198627347549E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5976720409858000E+0;
    b=0.1808370355053196E+0;
    v=0.4648659016801781E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6409956378989354E+0;
    b=0.2257852192301602E+0;
    v=0.4675502017157673E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5581222330827514E+0;
    b=0.4532173421637160E-1;
    v=0.4598494476455523E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6074705984161695E+0;
    b=0.9117488031840314E-1;
    v=0.4654916955152048E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6532272537379033E+0;
    b=0.1369294213140155E+0;
    v=0.4684709779505137E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6594761494500487E+0;
    b=0.4589901487275583E-1;
    v=0.4691445539106986E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 2702:

    v=0.2998675149888161E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.4077860529495355E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.2065562538818703E-1;
    v=0.1185349192520667E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5250918173022379E-1;
    v=0.1913408643425751E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.8993480082038376E-1;
    v=0.2452886577209897E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1306023924436019E+0;
    v=0.2862408183288702E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1732060388531418E+0;
    v=0.3178032258257357E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2168727084820249E+0;
    v=0.3422945667633690E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2609528309173586E+0;
    v=0.3612790520235922E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3049252927938952E+0;
    v=0.3758638229818521E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3483484138084404E+0;
    v=0.3868711798859953E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3908321549106406E+0;
    v=0.3949429933189938E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4320210071894814E+0;
    v=0.4006068107541156E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4715824795890053E+0;
    v=0.4043192149672723E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5091984794078453E+0;
    v=0.4064947495808078E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5445580145650803E+0;
    v=0.4075245619813152E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6072575796841768E+0;
    v=0.4076423540893566E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6339484505755803E+0;
    v=0.4074280862251555E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6570718257486958E+0;
    v=0.4074163756012244E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6762557330090709E+0;
    v=0.4077647795071246E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6911161696923790E+0;
    v=0.4084517552782530E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7012841911659961E+0;
    v=0.4092468459224052E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7064559272410020E+0;
    v=0.4097872687240906E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6123554989894765E-1;
    v=0.1738986811745028E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1533070348312393E+0;
    v=0.2659616045280191E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2563902605244206E+0;
    v=0.3240596008171533E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3629346991663361E+0;
    v=0.3621195964432943E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4683949968987538E+0;
    v=0.3868838330760539E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5694479240657952E+0;
    v=0.4018911532693111E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6634465430993955E+0;
    v=0.4089929432983252E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1033958573552305E+0;
    b=0.3034544009063584E-1;
    v=0.2279907527706409E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1473521412414395E+0;
    b=0.6618803044247135E-1;
    v=0.2715205490578897E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1924552158705967E+0;
    b=0.1054431128987715E+0;
    v=0.3057917896703976E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2381094362890328E+0;
    b=0.1468263551238858E+0;
    v=0.3326913052452555E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2838121707936760E+0;
    b=0.1894486108187886E+0;
    v=0.3537334711890037E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3291323133373415E+0;
    b=0.2326374238761579E+0;
    v=0.3700567500783129E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3736896978741460E+0;
    b=0.2758485808485768E+0;
    v=0.3825245372589122E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4171406040760013E+0;
    b=0.3186179331996921E+0;
    v=0.3918125171518296E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4591677985256915E+0;
    b=0.3605329796303794E+0;
    v=0.3984720419937579E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4994733831718418E+0;
    b=0.4012147253586509E+0;
    v=0.4029746003338211E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5377731830445096E+0;
    b=0.4403050025570692E+0;
    v=0.4057428632156627E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5737917830001331E+0;
    b=0.4774565904277483E+0;
    v=0.4071719274114857E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2027323586271389E+0;
    b=0.3544122504976147E-1;
    v=0.2990236950664119E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2516942375187273E+0;
    b=0.7418304388646328E-1;
    v=0.3262951734212878E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3000227995257181E+0;
    b=0.1150502745727186E+0;
    v=0.3482634608242413E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3474806691046342E+0;
    b=0.1571963371209364E+0;
    v=0.3656596681700892E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3938103180359209E+0;
    b=0.1999631877247100E+0;
    v=0.3791740467794218E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4387519590455703E+0;
    b=0.2428073457846535E+0;
    v=0.3894034450156905E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4820503960077787E+0;
    b=0.2852575132906155E+0;
    v=0.3968600245508371E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5234573778475101E+0;
    b=0.3268884208674639E+0;
    v=0.4019931351420050E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5627318647235282E+0;
    b=0.3673033321675939E+0;
    v=0.4052108801278599E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5996390607156954E+0;
    b=0.4061211551830290E+0;
    v=0.4068978613940934E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3084780753791947E+0;
    b=0.3860125523100059E-1;
    v=0.3454275351319704E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3589988275920223E+0;
    b=0.7928938987104867E-1;
    v=0.3629963537007920E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4078628415881973E+0;
    b=0.1212614643030087E+0;
    v=0.3770187233889873E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4549287258889735E+0;
    b=0.1638770827382693E+0;
    v=0.3878608613694378E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5000278512957279E+0;
    b=0.2065965798260176E+0;
    v=0.3959065270221274E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5429785044928199E+0;
    b=0.2489436378852235E+0;
    v=0.4015286975463570E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5835939850491711E+0;
    b=0.2904811368946891E+0;
    v=0.4050866785614717E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6216870353444856E+0;
    b=0.3307941957666609E+0;
    v=0.4069320185051913E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4151104662709091E+0;
    b=0.4064829146052554E-1;
    v=0.3760120964062763E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4649804275009218E+0;
    b=0.8258424547294755E-1;
    v=0.3870969564418064E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5124695757009662E+0;
    b=0.1251841962027289E+0;
    v=0.3955287790534055E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5574711100606224E+0;
    b=0.1679107505976331E+0;
    v=0.4015361911302668E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5998597333287227E+0;
    b=0.2102805057358715E+0;
    v=0.4053836986719548E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6395007148516600E+0;
    b=0.2518418087774107E+0;
    v=0.4073578673299117E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5188456224746252E+0;
    b=0.4194321676077518E-1;
    v=0.3954628379231406E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5664190707942778E+0;
    b=0.8457661551921499E-1;
    v=0.4017645508847530E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6110464353283153E+0;
    b=0.1273652932519396E+0;
    v=0.4059030348651293E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6526430302051563E+0;
    b=0.1698173239076354E+0;
    v=0.4080565809484880E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6167551880377548E+0;
    b=0.4266398851548864E-1;
    v=0.4063018753664651E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6607195418355383E+0;
    b=0.8551925814238349E-1;
    v=0.4087191292799671E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 3074:

    v=0.2599095953754734E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.3603134089687541E-3;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.3586067974412447E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.1886108518723392E-1;
    v=0.9831528474385880E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4800217244625303E-1;
    v=0.1605023107954450E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.8244922058397242E-1;
    v=0.2072200131464099E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1200408362484023E+0;
    v=0.2431297618814187E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1595773530809965E+0;
    v=0.2711819064496707E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2002635973434064E+0;
    v=0.2932762038321116E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2415127590139982E+0;
    v=0.3107032514197368E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2828584158458477E+0;
    v=0.3243808058921213E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3239091015338138E+0;
    v=0.3349899091374030E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3643225097962194E+0;
    v=0.3430580688505218E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4037897083691802E+0;
    v=0.3490124109290343E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4420247515194127E+0;
    v=0.3532148948561955E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4787572538464938E+0;
    v=0.3559862669062833E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5137265251275234E+0;
    v=0.3576224317551411E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5466764056654611E+0;
    v=0.3584050533086076E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6054859420813535E+0;
    v=0.3584903581373224E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6308106701764562E+0;
    v=0.3582991879040586E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6530369230179584E+0;
    v=0.3582371187963125E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6718609524611158E+0;
    v=0.3584353631122350E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6869676499894013E+0;
    v=0.3589120166517785E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6980467077240748E+0;
    v=0.3595445704531601E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7048241721250522E+0;
    v=0.3600943557111074E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5591105222058232E-1;
    v=0.1456447096742039E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1407384078513916E+0;
    v=0.2252370188283782E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2364035438976309E+0;
    v=0.2766135443474897E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3360602737818170E+0;
    v=0.3110729491500851E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4356292630054665E+0;
    v=0.3342506712303391E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5321569415256174E+0;
    v=0.3491981834026860E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6232956305040554E+0;
    v=0.3576003604348932E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.9469870086838469E-1;
    b=0.2778748387309470E-1;
    v=0.1921921305788564E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1353170300568141E+0;
    b=0.6076569878628364E-1;
    v=0.2301458216495632E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1771679481726077E+0;
    b=0.9703072762711040E-1;
    v=0.2604248549522893E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2197066664231751E+0;
    b=0.1354112458524762E+0;
    v=0.2845275425870697E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2624783557374927E+0;
    b=0.1750996479744100E+0;
    v=0.3036870897974840E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3050969521214442E+0;
    b=0.2154896907449802E+0;
    v=0.3188414832298066E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3472252637196021E+0;
    b=0.2560954625740152E+0;
    v=0.3307046414722089E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3885610219026360E+0;
    b=0.2965070050624096E+0;
    v=0.3398330969031360E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4288273776062765E+0;
    b=0.3363641488734497E+0;
    v=0.3466757899705373E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4677662471302948E+0;
    b=0.3753400029836788E+0;
    v=0.3516095923230054E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5051333589553359E+0;
    b=0.4131297522144286E+0;
    v=0.3549645184048486E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5406942145810492E+0;
    b=0.4494423776081795E+0;
    v=0.3570415969441392E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5742204122576457E+0;
    b=0.4839938958841502E+0;
    v=0.3581251798496118E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1865407027225188E+0;
    b=0.3259144851070796E-1;
    v=0.2543491329913348E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2321186453689432E+0;
    b=0.6835679505297343E-1;
    v=0.2786711051330776E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2773159142523882E+0;
    b=0.1062284864451989E+0;
    v=0.2985552361083679E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3219200192237254E+0;
    b=0.1454404409323047E+0;
    v=0.3145867929154039E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3657032593944029E+0;
    b=0.1854018282582510E+0;
    v=0.3273290662067609E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4084376778363622E+0;
    b=0.2256297412014750E+0;
    v=0.3372705511943501E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4499004945751427E+0;
    b=0.2657104425000896E+0;
    v=0.3448274437851510E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4898758141326335E+0;
    b=0.3052755487631557E+0;
    v=0.3503592783048583E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5281547442266309E+0;
    b=0.3439863920645423E+0;
    v=0.3541854792663162E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5645346989813992E+0;
    b=0.3815229456121914E+0;
    v=0.3565995517909428E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5988181252159848E+0;
    b=0.4175752420966734E+0;
    v=0.3578802078302898E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2850425424471603E+0;
    b=0.3562149509862536E-1;
    v=0.2958644592860982E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3324619433027876E+0;
    b=0.7330318886871096E-1;
    v=0.3119548129116835E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3785848333076282E+0;
    b=0.1123226296008472E+0;
    v=0.3250745225005984E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4232891028562115E+0;
    b=0.1521084193337708E+0;
    v=0.3355153415935208E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4664287050829722E+0;
    b=0.1921844459223610E+0;
    v=0.3435847568549328E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5078458493735726E+0;
    b=0.2321360989678303E+0;
    v=0.3495786831622488E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5473779816204180E+0;
    b=0.2715886486360520E+0;
    v=0.3537767805534621E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5848617133811376E+0;
    b=0.3101924707571355E+0;
    v=0.3564459815421428E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6201348281584888E+0;
    b=0.3476121052890973E+0;
    v=0.3578464061225468E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3852191185387871E+0;
    b=0.3763224880035108E-1;
    v=0.3239748762836212E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4325025061073423E+0;
    b=0.7659581935637135E-1;
    v=0.3345491784174287E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4778486229734490E+0;
    b=0.1163381306083900E+0;
    v=0.3429126177301782E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5211663693009000E+0;
    b=0.1563890598752899E+0;
    v=0.3492420343097421E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5623469504853703E+0;
    b=0.1963320810149200E+0;
    v=0.3537399050235257E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6012718188659246E+0;
    b=0.2357847407258738E+0;
    v=0.3566209152659172E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6378179206390117E+0;
    b=0.2743846121244060E+0;
    v=0.3581084321919782E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4836936460214534E+0;
    b=0.3895902610739024E-1;
    v=0.3426522117591512E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5293792562683797E+0;
    b=0.7871246819312640E-1;
    v=0.3491848770121379E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5726281253100033E+0;
    b=0.1187963808202981E+0;
    v=0.3539318235231476E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6133658776169068E+0;
    b=0.1587914708061787E+0;
    v=0.3570231438458694E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6515085491865307E+0;
    b=0.1983058575227646E+0;
    v=0.3586207335051714E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5778692716064976E+0;
    b=0.3977209689791542E-1;
    v=0.3541196205164025E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6207904288086192E+0;
    b=0.7990157592981152E-1;
    v=0.3574296911573953E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6608688171046802E+0;
    b=0.1199671308754309E+0;
    v=0.3591993279818963E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6656263089489130E+0;
    b=0.4015955957805969E-1;
    v=0.3595855034661997E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 3470:

    v=0.2040382730826330E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.3178149703889544E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.1721420832906233E-1;
    v=0.8288115128076110E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4408875374981770E-1;
    v=0.1360883192522954E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7594680813878681E-1;
    v=0.1766854454542662E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1108335359204799E+0;
    v=0.2083153161230153E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1476517054388567E+0;
    v=0.2333279544657158E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1856731870860615E+0;
    v=0.2532809539930247E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2243634099428821E+0;
    v=0.2692472184211158E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2633006881662727E+0;
    v=0.2819949946811885E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3021340904916283E+0;
    v=0.2920953593973030E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3405594048030089E+0;
    v=0.2999889782948352E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3783044434007372E+0;
    v=0.3060292120496902E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4151194767407910E+0;
    v=0.3105109167522192E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4507705766443257E+0;
    v=0.3136902387550312E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4850346056573187E+0;
    v=0.3157984652454632E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5176950817792470E+0;
    v=0.3170516518425422E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5485384240820989E+0;
    v=0.3176568425633755E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6039117238943308E+0;
    v=0.3177198411207062E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6279956655573113E+0;
    v=0.3175519492394733E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6493636169568952E+0;
    v=0.3174654952634756E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6677644117704504E+0;
    v=0.3175676415467654E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6829368572115624E+0;
    v=0.3178923417835410E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6946195818184121E+0;
    v=0.3183788287531909E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7025711542057026E+0;
    v=0.3188755151918807E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7066004767140119E+0;
    v=0.3191916889313849E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5132537689946062E-1;
    v=0.1231779611744508E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1297994661331225E+0;
    v=0.1924661373839880E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2188852049401307E+0;
    v=0.2380881867403424E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3123174824903457E+0;
    v=0.2693100663037885E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4064037620738195E+0;
    v=0.2908673382834366E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4984958396944782E+0;
    v=0.3053914619381535E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5864975046021365E+0;
    v=0.3143916684147777E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6686711634580175E+0;
    v=0.3187042244055363E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.8715738780835950E-1;
    b=0.2557175233367578E-1;
    v=0.1635219535869790E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1248383123134007E+0;
    b=0.5604823383376681E-1;
    v=0.1968109917696070E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1638062693383378E+0;
    b=0.8968568601900765E-1;
    v=0.2236754342249974E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2035586203373176E+0;
    b=0.1254086651976279E+0;
    v=0.2453186687017181E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2436798975293774E+0;
    b=0.1624780150162012E+0;
    v=0.2627551791580541E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2838207507773806E+0;
    b=0.2003422342683208E+0;
    v=0.2767654860152220E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3236787502217692E+0;
    b=0.2385628026255263E+0;
    v=0.2879467027765895E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3629849554840691E+0;
    b=0.2767731148783578E+0;
    v=0.2967639918918702E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4014948081992087E+0;
    b=0.3146542308245309E+0;
    v=0.3035900684660351E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4389818379260225E+0;
    b=0.3519196415895088E+0;
    v=0.3087338237298308E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4752331143674377E+0;
    b=0.3883050984023654E+0;
    v=0.3124608838860167E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5100457318374018E+0;
    b=0.4235613423908649E+0;
    v=0.3150084294226743E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5432238388954868E+0;
    b=0.4574484717196220E+0;
    v=0.3165958398598402E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5745758685072442E+0;
    b=0.4897311639255524E+0;
    v=0.3174320440957372E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1723981437592809E+0;
    b=0.3010630597881105E-1;
    v=0.2182188909812599E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2149553257844597E+0;
    b=0.6326031554204694E-1;
    v=0.2399727933921445E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2573256081247422E+0;
    b=0.9848566980258631E-1;
    v=0.2579796133514652E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2993163751238106E+0;
    b=0.1350835952384266E+0;
    v=0.2727114052623535E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3407238005148000E+0;
    b=0.1725184055442181E+0;
    v=0.2846327656281355E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3813454978483264E+0;
    b=0.2103559279730725E+0;
    v=0.2941491102051334E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4209848104423343E+0;
    b=0.2482278774554860E+0;
    v=0.3016049492136107E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4594519699996300E+0;
    b=0.2858099509982883E+0;
    v=0.3072949726175648E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4965640166185930E+0;
    b=0.3228075659915428E+0;
    v=0.3114768142886460E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5321441655571562E+0;
    b=0.3589459907204151E+0;
    v=0.3143823673666223E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5660208438582166E+0;
    b=0.3939630088864310E+0;
    v=0.3162269764661535E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5980264315964364E+0;
    b=0.4276029922949089E+0;
    v=0.3172164663759821E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2644215852350733E+0;
    b=0.3300939429072552E-1;
    v=0.2554575398967435E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3090113743443063E+0;
    b=0.6803887650078501E-1;
    v=0.2701704069135677E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3525871079197808E+0;
    b=0.1044326136206709E+0;
    v=0.2823693413468940E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3950418005354029E+0;
    b=0.1416751597517679E+0;
    v=0.2922898463214289E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4362475663430163E+0;
    b=0.1793408610504821E+0;
    v=0.3001829062162428E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4760661812145854E+0;
    b=0.2170630750175722E+0;
    v=0.3062890864542953E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5143551042512103E+0;
    b=0.2545145157815807E+0;
    v=0.3108328279264746E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5509709026935597E+0;
    b=0.2913940101706601E+0;
    v=0.3140243146201245E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5857711030329428E+0;
    b=0.3274169910910705E+0;
    v=0.3160638030977130E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6186149917404392E+0;
    b=0.3623081329317265E+0;
    v=0.3171462882206275E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3586894569557064E+0;
    b=0.3497354386450040E-1;
    v=0.2812388416031796E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4035266610019441E+0;
    b=0.7129736739757095E-1;
    v=0.2912137500288045E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4467775312332510E+0;
    b=0.1084758620193165E+0;
    v=0.2993241256502206E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4883638346608543E+0;
    b=0.1460915689241772E+0;
    v=0.3057101738983822E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5281908348434601E+0;
    b=0.1837790832369980E+0;
    v=0.3105319326251432E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5661542687149311E+0;
    b=0.2212075390874021E+0;
    v=0.3139565514428167E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6021450102031452E+0;
    b=0.2580682841160985E+0;
    v=0.3161543006806366E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6360520783610050E+0;
    b=0.2940656362094121E+0;
    v=0.3172985960613294E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4521611065087196E+0;
    b=0.3631055365867002E-1;
    v=0.2989400336901431E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4959365651560963E+0;
    b=0.7348318468484350E-1;
    v=0.3054555883947677E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5376815804038283E+0;
    b=0.1111087643812648E+0;
    v=0.3104764960807702E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5773314480243768E+0;
    b=0.1488226085145408E+0;
    v=0.3141015825977616E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6148113245575056E+0;
    b=0.1862892274135151E+0;
    v=0.3164520621159896E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6500407462842380E+0;
    b=0.2231909701714456E+0;
    v=0.3176652305912204E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5425151448707213E+0;
    b=0.3718201306118944E-1;
    v=0.3105097161023939E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5841860556907931E+0;
    b=0.7483616335067346E-1;
    v=0.3143014117890550E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6234632186851500E+0;
    b=0.1125990834266120E+0;
    v=0.3168172866287200E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6602934551848843E+0;
    b=0.1501303813157619E+0;
    v=0.3181401865570968E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6278573968375105E+0;
    b=0.3767559930245720E-1;
    v=0.3170663659156037E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6665611711264577E+0;
    b=0.7548443301360158E-1;
    v=0.3185447944625510E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 3890:

    v=0.1807395252196920E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.2848008782238827E-3;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.2836065837530581E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.1587876419858352E-1;
    v=0.7013149266673816E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4069193593751206E-1;
    v=0.1162798021956766E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7025888115257997E-1;
    v=0.1518728583972105E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1027495450028704E+0;
    v=0.1798796108216934E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1371457730893426E+0;
    v=0.2022593385972785E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1727758532671953E+0;
    v=0.2203093105575464E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2091492038929037E+0;
    v=0.2349294234299855E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2458813281751915E+0;
    v=0.2467682058747003E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2826545859450066E+0;
    v=0.2563092683572224E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3191957291799622E+0;
    v=0.2639253896763318E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3552621469299578E+0;
    v=0.2699137479265108E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3906329503406230E+0;
    v=0.2745196420166739E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4251028614093031E+0;
    v=0.2779529197397593E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4584777520111870E+0;
    v=0.2803996086684265E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4905711358710193E+0;
    v=0.2820302356715842E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5212011669847385E+0;
    v=0.2830056747491068E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5501878488737995E+0;
    v=0.2834808950776839E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6025037877479342E+0;
    v=0.2835282339078929E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6254572689549016E+0;
    v=0.2833819267065800E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6460107179528248E+0;
    v=0.2832858336906784E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6639541138154251E+0;
    v=0.2833268235451244E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6790688515667495E+0;
    v=0.2835432677029253E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6911338580371512E+0;
    v=0.2839091722743049E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6999385956126490E+0;
    v=0.2843308178875841E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7053037748656896E+0;
    v=0.2846703550533846E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4732224387180115E-1;
    v=0.1051193406971900E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1202100529326803E+0;
    v=0.1657871838796974E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2034304820664855E+0;
    v=0.2064648113714232E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2912285643573002E+0;
    v=0.2347942745819741E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3802361792726768E+0;
    v=0.2547775326597726E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4680598511056146E+0;
    v=0.2686876684847025E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5528151052155599E+0;
    v=0.2778665755515867E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6329386307803041E+0;
    v=0.2830996616782929E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.8056516651369069E-1;
    b=0.2363454684003124E-1;
    v=0.1403063340168372E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1156476077139389E+0;
    b=0.5191291632545936E-1;
    v=0.1696504125939477E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1520473382760421E+0;
    b=0.8322715736994519E-1;
    v=0.1935787242745390E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1892986699745931E+0;
    b=0.1165855667993712E+0;
    v=0.2130614510521968E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2270194446777792E+0;
    b=0.1513077167409504E+0;
    v=0.2289381265931048E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2648908185093273E+0;
    b=0.1868882025807859E+0;
    v=0.2418630292816186E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3026389259574136E+0;
    b=0.2229277629776224E+0;
    v=0.2523400495631193E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3400220296151384E+0;
    b=0.2590951840746235E+0;
    v=0.2607623973449605E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3768217953335510E+0;
    b=0.2951047291750847E+0;
    v=0.2674441032689209E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4128372900921884E+0;
    b=0.3307019714169930E+0;
    v=0.2726432360343356E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4478807131815630E+0;
    b=0.3656544101087634E+0;
    v=0.2765787685924545E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4817742034089257E+0;
    b=0.3997448951939695E+0;
    v=0.2794428690642224E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5143472814653344E+0;
    b=0.4327667110812024E+0;
    v=0.2814099002062895E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5454346213905650E+0;
    b=0.4645196123532293E+0;
    v=0.2826429531578994E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5748739313170252E+0;
    b=0.4948063555703345E+0;
    v=0.2832983542550884E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1599598738286342E+0;
    b=0.2792357590048985E-1;
    v=0.1886695565284976E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1998097412500951E+0;
    b=0.5877141038139065E-1;
    v=0.2081867882748234E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2396228952566202E+0;
    b=0.9164573914691377E-1;
    v=0.2245148680600796E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2792228341097746E+0;
    b=0.1259049641962687E+0;
    v=0.2380370491511872E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3184251107546741E+0;
    b=0.1610594823400863E+0;
    v=0.2491398041852455E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3570481164426244E+0;
    b=0.1967151653460898E+0;
    v=0.2581632405881230E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3949164710492144E+0;
    b=0.2325404606175168E+0;
    v=0.2653965506227417E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4318617293970503E+0;
    b=0.2682461141151439E+0;
    v=0.2710857216747087E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4677221009931678E+0;
    b=0.3035720116011973E+0;
    v=0.2754434093903659E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5023417939270955E+0;
    b=0.3382781859197439E+0;
    v=0.2786579932519380E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5355701836636128E+0;
    b=0.3721383065625942E+0;
    v=0.2809011080679474E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5672608451328771E+0;
    b=0.4049346360466055E+0;
    v=0.2823336184560987E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5972704202540162E+0;
    b=0.4364538098633802E+0;
    v=0.2831101175806309E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2461687022333596E+0;
    b=0.3070423166833368E-1;
    v=0.2221679970354546E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2881774566286831E+0;
    b=0.6338034669281885E-1;
    v=0.2356185734270703E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3293963604116978E+0;
    b=0.9742862487067941E-1;
    v=0.2469228344805590E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3697303822241377E+0;
    b=0.1323799532282290E+0;
    v=0.2562726348642046E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4090663023135127E+0;
    b=0.1678497018129336E+0;
    v=0.2638756726753028E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4472819355411712E+0;
    b=0.2035095105326114E+0;
    v=0.2699311157390862E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4842513377231437E+0;
    b=0.2390692566672091E+0;
    v=0.2746233268403837E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5198477629962928E+0;
    b=0.2742649818076149E+0;
    v=0.2781225674454771E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5539453011883145E+0;
    b=0.3088503806580094E+0;
    v=0.2805881254045684E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5864196762401251E+0;
    b=0.3425904245906614E+0;
    v=0.2821719877004913E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6171484466668390E+0;
    b=0.3752562294789468E+0;
    v=0.2830222502333124E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3350337830565727E+0;
    b=0.3261589934634747E-1;
    v=0.2457995956744870E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3775773224758284E+0;
    b=0.6658438928081572E-1;
    v=0.2551474407503706E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4188155229848973E+0;
    b=0.1014565797157954E+0;
    v=0.2629065335195311E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4586805892009344E+0;
    b=0.1368573320843822E+0;
    v=0.2691900449925075E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4970895714224235E+0;
    b=0.1724614851951608E+0;
    v=0.2741275485754276E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5339505133960747E+0;
    b=0.2079779381416412E+0;
    v=0.2778530970122595E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5691665792531440E+0;
    b=0.2431385788322288E+0;
    v=0.2805010567646741E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6026387682680377E+0;
    b=0.2776901883049853E+0;
    v=0.2822055834031040E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6342676150163307E+0;
    b=0.3113881356386632E+0;
    v=0.2831016901243473E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4237951119537067E+0;
    b=0.3394877848664351E-1;
    v=0.2624474901131803E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4656918683234929E+0;
    b=0.6880219556291447E-1;
    v=0.2688034163039377E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5058857069185980E+0;
    b=0.1041946859721635E+0;
    v=0.2738932751287636E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5443204666713996E+0;
    b=0.1398039738736393E+0;
    v=0.2777944791242523E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5809298813759742E+0;
    b=0.1753373381196155E+0;
    v=0.2806011661660987E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6156416039447128E+0;
    b=0.2105215793514010E+0;
    v=0.2824181456597460E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6483801351066604E+0;
    b=0.2450953312157051E+0;
    v=0.2833585216577828E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5103616577251688E+0;
    b=0.3485560643800719E-1;
    v=0.2738165236962878E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5506738792580681E+0;
    b=0.7026308631512033E-1;
    v=0.2778365208203180E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5889573040995292E+0;
    b=0.1059035061296403E+0;
    v=0.2807852940418966E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6251641589516930E+0;
    b=0.1414823925236026E+0;
    v=0.2827245949674705E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6592414921570178E+0;
    b=0.1767207908214530E+0;
    v=0.2837342344829828E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5930314017533384E+0;
    b=0.3542189339561672E-1;
    v=0.2809233907610981E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6309812253390175E+0;
    b=0.7109574040369549E-1;
    v=0.2829930809742694E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6666296011353230E+0;
    b=0.1067259792282730E+0;
    v=0.2841097874111479E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6703715271049922E+0;
    b=0.3569455268820809E-1;
    v=0.2843455206008783E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 4334:

    v=0.1449063022537883E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.2546377329828424E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.1462896151831013E-1;
    v=0.6018432961087496E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3769840812493139E-1;
    v=0.1002286583263673E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6524701904096891E-1;
    v=0.1315222931028093E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.9560543416134648E-1;
    v=0.1564213746876724E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1278335898929198E+0;
    v=0.1765118841507736E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1613096104466031E+0;
    v=0.1928737099311080E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1955806225745371E+0;
    v=0.2062658534263270E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2302935218498028E+0;
    v=0.2172395445953787E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2651584344113027E+0;
    v=0.2262076188876047E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2999276825183209E+0;
    v=0.2334885699462397E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3343828669718798E+0;
    v=0.2393355273179203E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3683265013750518E+0;
    v=0.2439559200468863E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4015763206518108E+0;
    v=0.2475251866060002E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4339612026399770E+0;
    v=0.2501965558158773E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4653180651114582E+0;
    v=0.2521081407925925E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4954893331080803E+0;
    v=0.2533881002388081E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5243207068924930E+0;
    v=0.2541582900848261E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5516590479041704E+0;
    v=0.2545365737525860E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6012371927804176E+0;
    v=0.2545726993066799E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6231574466449819E+0;
    v=0.2544456197465555E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6429416514181271E+0;
    v=0.2543481596881064E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6604124272943595E+0;
    v=0.2543506451429194E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6753851470408250E+0;
    v=0.2544905675493763E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6876717970626160E+0;
    v=0.2547611407344429E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6970895061319234E+0;
    v=0.2551060375448869E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7034746912553310E+0;
    v=0.2554291933816039E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7067017217542295E+0;
    v=0.2556255710686343E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4382223501131123E-1;
    v=0.9041339695118195E-4;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1117474077400006E+0;
    v=0.1438426330079022E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1897153252911440E+0;
    v=0.1802523089820518E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2724023009910331E+0;
    v=0.2060052290565496E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3567163308709902E+0;
    v=0.2245002248967466E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4404784483028087E+0;
    v=0.2377059847731150E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5219833154161411E+0;
    v=0.2468118955882525E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5998179868977553E+0;
    v=0.2525410872966528E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6727803154548222E+0;
    v=0.2553101409933397E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.7476563943166086E-1;
    b=0.2193168509461185E-1;
    v=0.1212879733668632E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1075341482001416E+0;
    b=0.4826419281533887E-1;
    v=0.1472872881270931E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1416344885203259E+0;
    b=0.7751191883575742E-1;
    v=0.1686846601010828E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1766325315388586E+0;
    b=0.1087558139247680E+0;
    v=0.1862698414660208E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2121744174481514E+0;
    b=0.1413661374253096E+0;
    v=0.2007430956991861E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2479669443408145E+0;
    b=0.1748768214258880E+0;
    v=0.2126568125394796E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2837600452294113E+0;
    b=0.2089216406612073E+0;
    v=0.2224394603372113E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3193344933193984E+0;
    b=0.2431987685545972E+0;
    v=0.2304264522673135E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3544935442438745E+0;
    b=0.2774497054377770E+0;
    v=0.2368854288424087E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3890571932288154E+0;
    b=0.3114460356156915E+0;
    v=0.2420352089461772E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4228581214259090E+0;
    b=0.3449806851913012E+0;
    v=0.2460597113081295E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4557387211304052E+0;
    b=0.3778618641248256E+0;
    v=0.2491181912257687E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4875487950541643E+0;
    b=0.4099086391698978E+0;
    v=0.2513528194205857E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5181436529962997E+0;
    b=0.4409474925853973E+0;
    v=0.2528943096693220E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5473824095600661E+0;
    b=0.4708094517711291E+0;
    v=0.2538660368488136E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5751263398976174E+0;
    b=0.4993275140354637E+0;
    v=0.2543868648299022E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1489515746840028E+0;
    b=0.2599381993267017E-1;
    v=0.1642595537825183E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1863656444351767E+0;
    b=0.5479286532462190E-1;
    v=0.1818246659849308E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2238602880356348E+0;
    b=0.8556763251425254E-1;
    v=0.1966565649492420E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2612723375728160E+0;
    b=0.1177257802267011E+0;
    v=0.2090677905657991E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2984332990206190E+0;
    b=0.1508168456192700E+0;
    v=0.2193820409510504E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3351786584663333E+0;
    b=0.1844801892177727E+0;
    v=0.2278870827661928E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3713505522209120E+0;
    b=0.2184145236087598E+0;
    v=0.2348283192282090E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4067981098954663E+0;
    b=0.2523590641486229E+0;
    v=0.2404139755581477E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4413769993687534E+0;
    b=0.2860812976901373E+0;
    v=0.2448227407760734E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4749487182516394E+0;
    b=0.3193686757808996E+0;
    v=0.2482110455592573E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5073798105075426E+0;
    b=0.3520226949547602E+0;
    v=0.2507192397774103E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5385410448878654E+0;
    b=0.3838544395667890E+0;
    v=0.2524765968534880E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5683065353670530E+0;
    b=0.4146810037640963E+0;
    v=0.2536052388539425E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5965527620663510E+0;
    b=0.4443224094681121E+0;
    v=0.2542230588033068E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2299227700856157E+0;
    b=0.2865757664057584E-1;
    v=0.1944817013047896E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2695752998553267E+0;
    b=0.5923421684485993E-1;
    v=0.2067862362746635E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3086178716611389E+0;
    b=0.9117817776057715E-1;
    v=0.2172440734649114E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3469649871659077E+0;
    b=0.1240593814082605E+0;
    v=0.2260125991723423E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3845153566319655E+0;
    b=0.1575272058259175E+0;
    v=0.2332655008689523E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4211600033403215E+0;
    b=0.1912845163525413E+0;
    v=0.2391699681532458E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4567867834329882E+0;
    b=0.2250710177858171E+0;
    v=0.2438801528273928E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4912829319232061E+0;
    b=0.2586521303440910E+0;
    v=0.2475370504260665E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5245364793303812E+0;
    b=0.2918112242865407E+0;
    v=0.2502707235640574E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5564369788915756E+0;
    b=0.3243439239067890E+0;
    v=0.2522031701054241E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5868757697775287E+0;
    b=0.3560536787835351E+0;
    v=0.2534511269978784E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6157458853519617E+0;
    b=0.3867480821242581E+0;
    v=0.2541284914955151E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3138461110672113E+0;
    b=0.3051374637507278E-1;
    v=0.2161509250688394E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3542495872050569E+0;
    b=0.6237111233730755E-1;
    v=0.2248778513437852E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3935751553120181E+0;
    b=0.9516223952401907E-1;
    v=0.2322388803404617E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4317634668111147E+0;
    b=0.1285467341508517E+0;
    v=0.2383265471001355E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4687413842250821E+0;
    b=0.1622318931656033E+0;
    v=0.2432476675019525E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5044274237060283E+0;
    b=0.1959581153836453E+0;
    v=0.2471122223750674E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5387354077925727E+0;
    b=0.2294888081183837E+0;
    v=0.2500291752486870E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5715768898356105E+0;
    b=0.2626031152713945E+0;
    v=0.2521055942764682E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6028627200136111E+0;
    b=0.2950904075286713E+0;
    v=0.2534472785575503E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6325039812653463E+0;
    b=0.3267458451113286E+0;
    v=0.2541599713080121E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3981986708423407E+0;
    b=0.3183291458749821E-1;
    v=0.2317380975862936E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4382791182133300E+0;
    b=0.6459548193880908E-1;
    v=0.2378550733719775E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4769233057218166E+0;
    b=0.9795757037087952E-1;
    v=0.2428884456739118E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5140823911194238E+0;
    b=0.1316307235126655E+0;
    v=0.2469002655757292E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5496977833862983E+0;
    b=0.1653556486358704E+0;
    v=0.2499657574265851E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5837047306512727E+0;
    b=0.1988931724126510E+0;
    v=0.2521676168486082E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6160349566926879E+0;
    b=0.2320174581438950E+0;
    v=0.2535935662645334E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6466185353209440E+0;
    b=0.2645106562168662E+0;
    v=0.2543356743363214E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4810835158795404E+0;
    b=0.3275917807743992E-1;
    v=0.2427353285201535E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5199925041324341E+0;
    b=0.6612546183967181E-1;
    v=0.2468258039744386E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5571717692207494E+0;
    b=0.9981498331474143E-1;
    v=0.2500060956440310E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5925789250836378E+0;
    b=0.1335687001410374E+0;
    v=0.2523238365420979E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6261658523859670E+0;
    b=0.1671444402896463E+0;
    v=0.2538399260252846E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6578811126669331E+0;
    b=0.2003106382156076E+0;
    v=0.2546255927268069E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5609624612998100E+0;
    b=0.3337500940231335E-1;
    v=0.2500583360048449E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5979959659984670E+0;
    b=0.6708750335901803E-1;
    v=0.2524777638260203E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6330523711054002E+0;
    b=0.1008792126424850E+0;
    v=0.2540951193860656E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6660960998103972E+0;
    b=0.1345050343171794E+0;
    v=0.2549524085027472E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6365384364585819E+0;
    b=0.3372799460737052E-1;
    v=0.2542569507009158E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6710994302899275E+0;
    b=0.6755249309678028E-1;
    v=0.2552114127580376E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 4802:

    v=0.9687521879420705E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.2307897895367918E-3;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.2297310852498558E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.2335728608887064E-1;
    v=0.7386265944001919E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4352987836550653E-1;
    v=0.8257977698542210E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6439200521088801E-1;
    v=0.9706044762057630E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.9003943631993181E-1;
    v=0.1302393847117003E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1196706615548473E+0;
    v=0.1541957004600968E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1511715412838134E+0;
    v=0.1704459770092199E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1835982828503801E+0;
    v=0.1827374890942906E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2165081259155405E+0;
    v=0.1926360817436107E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2496208720417563E+0;
    v=0.2008010239494833E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2827200673567900E+0;
    v=0.2075635983209175E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3156190823994346E+0;
    v=0.2131306638690909E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3481476793749115E+0;
    v=0.2176562329937335E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3801466086947226E+0;
    v=0.2212682262991018E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4114652119634011E+0;
    v=0.2240799515668565E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4419598786519751E+0;
    v=0.2261959816187525E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4714925949329543E+0;
    v=0.2277156368808855E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4999293972879466E+0;
    v=0.2287351772128336E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5271387221431248E+0;
    v=0.2293490814084085E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5529896780837761E+0;
    v=0.2296505312376273E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6000856099481712E+0;
    v=0.2296793832318756E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6210562192785175E+0;
    v=0.2295785443842974E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6401165879934240E+0;
    v=0.2295017931529102E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6571144029244334E+0;
    v=0.2295059638184868E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6718910821718863E+0;
    v=0.2296232343237362E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6842845591099010E+0;
    v=0.2298530178740771E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6941353476269816E+0;
    v=0.2301579790280501E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7012965242212991E+0;
    v=0.2304690404996513E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7056471428242644E+0;
    v=0.2307027995907102E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4595557643585895E-1;
    v=0.9312274696671092E-4;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1049316742435023E+0;
    v=0.1199919385876926E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1773548879549274E+0;
    v=0.1598039138877690E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2559071411236127E+0;
    v=0.1822253763574900E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3358156837985898E+0;
    v=0.1988579593655040E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4155835743763893E+0;
    v=0.2112620102533307E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4937894296167472E+0;
    v=0.2201594887699007E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5691569694793316E+0;
    v=0.2261622590895036E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6405840854894251E+0;
    v=0.2296458453435705E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.7345133894143348E-1;
    b=0.2177844081486067E-1;
    v=0.1006006990267000E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1009859834044931E+0;
    b=0.4590362185775188E-1;
    v=0.1227676689635876E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1324289619748758E+0;
    b=0.7255063095690877E-1;
    v=0.1467864280270117E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1654272109607127E+0;
    b=0.1017825451960684E+0;
    v=0.1644178912101232E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1990767186776461E+0;
    b=0.1325652320980364E+0;
    v=0.1777664890718961E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2330125945523278E+0;
    b=0.1642765374496765E+0;
    v=0.1884825664516690E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2670080611108287E+0;
    b=0.1965360374337889E+0;
    v=0.1973269246453848E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3008753376294316E+0;
    b=0.2290726770542238E+0;
    v=0.2046767775855328E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3344475596167860E+0;
    b=0.2616645495370823E+0;
    v=0.2107600125918040E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3675709724070786E+0;
    b=0.2941150728843141E+0;
    v=0.2157416362266829E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4001000887587812E+0;
    b=0.3262440400919066E+0;
    v=0.2197557816920721E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4318956350436028E+0;
    b=0.3578835350611916E+0;
    v=0.2229192611835437E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4628239056795531E+0;
    b=0.3888751854043678E+0;
    v=0.2253385110212775E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4927563229773636E+0;
    b=0.4190678003222840E+0;
    v=0.2271137107548774E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5215687136707969E+0;
    b=0.4483151836883852E+0;
    v=0.2283414092917525E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5491402346984905E+0;
    b=0.4764740676087880E+0;
    v=0.2291161673130077E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5753520160126075E+0;
    b=0.5034021310998277E+0;
    v=0.2295313908576598E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1388326356417754E+0;
    b=0.2435436510372806E-1;
    v=0.1438204721359031E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1743686900537244E+0;
    b=0.5118897057342652E-1;
    v=0.1607738025495257E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2099737037950268E+0;
    b=0.8014695048539634E-1;
    v=0.1741483853528379E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2454492590908548E+0;
    b=0.1105117874155699E+0;
    v=0.1851918467519151E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2807219257864278E+0;
    b=0.1417950531570966E+0;
    v=0.1944628638070613E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3156842271975842E+0;
    b=0.1736604945719597E+0;
    v=0.2022495446275152E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3502090945177752E+0;
    b=0.2058466324693981E+0;
    v=0.2087462382438514E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3841684849519686E+0;
    b=0.2381284261195919E+0;
    v=0.2141074754818308E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4174372367906016E+0;
    b=0.2703031270422569E+0;
    v=0.2184640913748162E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4498926465011892E+0;
    b=0.3021845683091309E+0;
    v=0.2219309165220329E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4814146229807701E+0;
    b=0.3335993355165720E+0;
    v=0.2246123118340624E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5118863625734701E+0;
    b=0.3643833735518232E+0;
    v=0.2266062766915125E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5411947455119144E+0;
    b=0.3943789541958179E+0;
    v=0.2280072952230796E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5692301500357246E+0;
    b=0.4234320144403542E+0;
    v=0.2289082025202583E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5958857204139576E+0;
    b=0.4513897947419260E+0;
    v=0.2294012695120025E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2156270284785766E+0;
    b=0.2681225755444491E-1;
    v=0.1722434488736947E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2532385054909710E+0;
    b=0.5557495747805614E-1;
    v=0.1830237421455091E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2902564617771537E+0;
    b=0.8569368062950249E-1;
    v=0.1923855349997633E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3266979823143256E+0;
    b=0.1167367450324135E+0;
    v=0.2004067861936271E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3625039627493614E+0;
    b=0.1483861994003304E+0;
    v=0.2071817297354263E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3975838937548699E+0;
    b=0.1803821503011405E+0;
    v=0.2128250834102103E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4318396099009774E+0;
    b=0.2124962965666424E+0;
    v=0.2174513719440102E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4651706555732742E+0;
    b=0.2445221837805913E+0;
    v=0.2211661839150214E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4974752649620969E+0;
    b=0.2762701224322987E+0;
    v=0.2240665257813102E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5286517579627517E+0;
    b=0.3075627775211328E+0;
    v=0.2262439516632620E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5586001195731895E+0;
    b=0.3382311089826877E+0;
    v=0.2277874557231869E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5872229902021319E+0;
    b=0.3681108834741399E+0;
    v=0.2287854314454994E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6144258616235123E+0;
    b=0.3970397446872839E+0;
    v=0.2293268499615575E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2951676508064861E+0;
    b=0.2867499538750441E-1;
    v=0.1912628201529828E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3335085485472725E+0;
    b=0.5867879341903510E-1;
    v=0.1992499672238701E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3709561760636381E+0;
    b=0.8961099205022284E-1;
    v=0.2061275533454027E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4074722861667498E+0;
    b=0.1211627927626297E+0;
    v=0.2119318215968572E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4429923648839117E+0;
    b=0.1530748903554898E+0;
    v=0.2167416581882652E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4774428052721736E+0;
    b=0.1851176436721877E+0;
    v=0.2206430730516600E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5107446539535904E+0;
    b=0.2170829107658179E+0;
    v=0.2237186938699523E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5428151370542935E+0;
    b=0.2487786689026271E+0;
    v=0.2260480075032884E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5735699292556964E+0;
    b=0.2800239952795016E+0;
    v=0.2277098884558542E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6029253794562866E+0;
    b=0.3106445702878119E+0;
    v=0.2287845715109671E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6307998987073145E+0;
    b=0.3404689500841194E+0;
    v=0.2293547268236294E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3752652273692719E+0;
    b=0.2997145098184479E-1;
    v=0.2056073839852528E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4135383879344028E+0;
    b=0.6086725898678011E-1;
    v=0.2114235865831876E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4506113885153907E+0;
    b=0.9238849548435643E-1;
    v=0.2163175629770551E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4864401554606072E+0;
    b=0.1242786603851851E+0;
    v=0.2203392158111650E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5209708076611709E+0;
    b=0.1563086731483386E+0;
    v=0.2235473176847839E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5541422135830122E+0;
    b=0.1882696509388506E+0;
    v=0.2260024141501235E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5858880915113817E+0;
    b=0.2199672979126059E+0;
    v=0.2277675929329182E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6161399390603444E+0;
    b=0.2512165482924867E+0;
    v=0.2289102112284834E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6448296482255090E+0;
    b=0.2818368701871888E+0;
    v=0.2295027954625118E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4544796274917948E+0;
    b=0.3088970405060312E-1;
    v=0.2161281589879992E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4919389072146628E+0;
    b=0.6240947677636835E-1;
    v=0.2201980477395102E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5279313026985183E+0;
    b=0.9430706144280313E-1;
    v=0.2234952066593166E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5624169925571135E+0;
    b=0.1263547818770374E+0;
    v=0.2260540098520838E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5953484627093287E+0;
    b=0.1583430788822594E+0;
    v=0.2279157981899988E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6266730715339185E+0;
    b=0.1900748462555988E+0;
    v=0.2291296918565571E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6563363204278871E+0;
    b=0.2213599519592567E+0;
    v=0.2297533752536649E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5314574716585696E+0;
    b=0.3152508811515374E-1;
    v=0.2234927356465995E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5674614932298185E+0;
    b=0.6343865291465561E-1;
    v=0.2261288012985219E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6017706004970264E+0;
    b=0.9551503504223951E-1;
    v=0.2280818160923688E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6343471270264178E+0;
    b=0.1275440099801196E+0;
    v=0.2293773295180159E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6651494599127802E+0;
    b=0.1593252037671960E+0;
    v=0.2300528767338634E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6050184986005704E+0;
    b=0.3192538338496105E-1;
    v=0.2281893855065666E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6390163550880400E+0;
    b=0.6402824353962306E-1;
    v=0.2295720444840727E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6711199107088448E+0;
    b=0.9609805077002909E-1;
    v=0.2303227649026753E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6741354429572275E+0;
    b=0.3211853196273233E-1;
    v=0.2304831913227114E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 5294:

    v=0.9080510764308163E-4;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.2084824361987793E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.2303261686261450E-1;
    v=0.5011105657239616E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3757208620162394E-1;
    v=0.5942520409683854E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5821912033821852E-1;
    v=0.9564394826109721E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.8403127529194872E-1;
    v=0.1185530657126338E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1122927798060578E+0;
    v=0.1364510114230331E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1420125319192987E+0;
    v=0.1505828825605415E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1726396437341978E+0;
    v=0.1619298749867023E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2038170058115696E+0;
    v=0.1712450504267789E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2352849892876508E+0;
    v=0.1789891098164999E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2668363354312461E+0;
    v=0.1854474955629795E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2982941279900452E+0;
    v=0.1908148636673661E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3295002922087076E+0;
    v=0.1952377405281833E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3603094918363593E+0;
    v=0.1988349254282232E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3905857895173920E+0;
    v=0.2017079807160050E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4202005758160837E+0;
    v=0.2039473082709094E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4490310061597227E+0;
    v=0.2056360279288953E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4769586160311491E+0;
    v=0.2068525823066865E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5038679887049750E+0;
    v=0.2076724877534488E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5296454286519961E+0;
    v=0.2081694278237885E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5541776207164850E+0;
    v=0.2084157631219326E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5990467321921213E+0;
    v=0.2084381531128593E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6191467096294587E+0;
    v=0.2083476277129307E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6375251212901849E+0;
    v=0.2082686194459732E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6540514381131168E+0;
    v=0.2082475686112415E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6685899064391510E+0;
    v=0.2083139860289915E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6810013009681648E+0;
    v=0.2084745561831237E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6911469578730340E+0;
    v=0.2087091313375890E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6988956915141736E+0;
    v=0.2089718413297697E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7041335794868720E+0;
    v=0.2092003303479793E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7067754398018567E+0;
    v=0.2093336148263241E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3840368707853623E-1;
    v=0.7591708117365267E-4;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.9835485954117399E-1;
    v=0.1083383968169186E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1665774947612998E+0;
    v=0.1403019395292510E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2405702335362910E+0;
    v=0.1615970179286436E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3165270770189046E+0;
    v=0.1771144187504911E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3927386145645443E+0;
    v=0.1887760022988168E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4678825918374656E+0;
    v=0.1973474670768214E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5408022024266935E+0;
    v=0.2033787661234659E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6104967445752438E+0;
    v=0.2072343626517331E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6760910702685738E+0;
    v=0.2091177834226918E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6655644120217392E-1;
    b=0.1936508874588424E-1;
    v=0.9316684484675566E-4;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.9446246161270182E-1;
    b=0.4252442002115869E-1;
    v=0.1116193688682976E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1242651925452509E+0;
    b=0.6806529315354374E-1;
    v=0.1298623551559414E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1553438064846751E+0;
    b=0.9560957491205369E-1;
    v=0.1450236832456426E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1871137110542670E+0;
    b=0.1245931657452888E+0;
    v=0.1572719958149914E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2192612628836257E+0;
    b=0.1545385828778978E+0;
    v=0.1673234785867195E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2515682807206955E+0;
    b=0.1851004249723368E+0;
    v=0.1756860118725188E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2838535866287290E+0;
    b=0.2160182608272384E+0;
    v=0.1826776290439367E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3159578817528521E+0;
    b=0.2470799012277111E+0;
    v=0.1885116347992865E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3477370882791392E+0;
    b=0.2781014208986402E+0;
    v=0.1933457860170574E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3790576960890540E+0;
    b=0.3089172523515731E+0;
    v=0.1973060671902064E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4097938317810200E+0;
    b=0.3393750055472244E+0;
    v=0.2004987099616311E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4398256572859637E+0;
    b=0.3693322470987730E+0;
    v=0.2030170909281499E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4690384114718480E+0;
    b=0.3986541005609877E+0;
    v=0.2049461460119080E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4973216048301053E+0;
    b=0.4272112491408562E+0;
    v=0.2063653565200186E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5245681526132446E+0;
    b=0.4548781735309936E+0;
    v=0.2073507927381027E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5506733911803888E+0;
    b=0.4815315355023251E+0;
    v=0.2079764593256122E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5755339829522475E+0;
    b=0.5070486445801855E+0;
    v=0.2083150534968778E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1305472386056362E+0;
    b=0.2284970375722366E-1;
    v=0.1262715121590664E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1637327908216477E+0;
    b=0.4812254338288384E-1;
    v=0.1414386128545972E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1972734634149637E+0;
    b=0.7531734457511935E-1;
    v=0.1538740401313898E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2308694653110130E+0;
    b=0.1039043639882017E+0;
    v=0.1642434942331432E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2643899218338160E+0;
    b=0.1334526587117626E+0;
    v=0.1729790609237496E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2977171599622171E+0;
    b=0.1636414868936382E+0;
    v=0.1803505190260828E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3307293903032310E+0;
    b=0.1942195406166568E+0;
    v=0.1865475350079657E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3633069198219073E+0;
    b=0.2249752879943753E+0;
    v=0.1917182669679069E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3953346955922727E+0;
    b=0.2557218821820032E+0;
    v=0.1959851709034382E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4267018394184914E+0;
    b=0.2862897925213193E+0;
    v=0.1994529548117882E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4573009622571704E+0;
    b=0.3165224536636518E+0;
    v=0.2022138911146548E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4870279559856109E+0;
    b=0.3462730221636496E+0;
    v=0.2043518024208592E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5157819581450322E+0;
    b=0.3754016870282835E+0;
    v=0.2059450313018110E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5434651666465393E+0;
    b=0.4037733784993613E+0;
    v=0.2070685715318472E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5699823887764627E+0;
    b=0.4312557784139123E+0;
    v=0.2077955310694373E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5952403350947741E+0;
    b=0.4577175367122110E+0;
    v=0.2081980387824712E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2025152599210369E+0;
    b=0.2520253617719557E-1;
    v=0.1521318610377956E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2381066653274425E+0;
    b=0.5223254506119000E-1;
    v=0.1622772720185755E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2732823383651612E+0;
    b=0.8060669688588620E-1;
    v=0.1710498139420709E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3080137692611118E+0;
    b=0.1099335754081255E+0;
    v=0.1785911149448736E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3422405614587601E+0;
    b=0.1399120955959857E+0;
    v=0.1850125313687736E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3758808773890420E+0;
    b=0.1702977801651705E+0;
    v=0.1904229703933298E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4088458383438932E+0;
    b=0.2008799256601680E+0;
    v=0.1949259956121987E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4410450550841152E+0;
    b=0.2314703052180836E+0;
    v=0.1986161545363960E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4723879420561312E+0;
    b=0.2618972111375892E+0;
    v=0.2015790585641370E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5027843561874343E+0;
    b=0.2920013195600270E+0;
    v=0.2038934198707418E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5321453674452458E+0;
    b=0.3216322555190551E+0;
    v=0.2056334060538251E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5603839113834030E+0;
    b=0.3506456615934198E+0;
    v=0.2068705959462289E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5874150706875146E+0;
    b=0.3789007181306267E+0;
    v=0.2076753906106002E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6131559381660038E+0;
    b=0.4062580170572782E+0;
    v=0.2081179391734803E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2778497016394506E+0;
    b=0.2696271276876226E-1;
    v=0.1700345216228943E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3143733562261912E+0;
    b=0.5523469316960465E-1;
    v=0.1774906779990410E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3501485810261827E+0;
    b=0.8445193201626464E-1;
    v=0.1839659377002642E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3851430322303653E+0;
    b=0.1143263119336083E+0;
    v=0.1894987462975169E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4193013979470415E+0;
    b=0.1446177898344475E+0;
    v=0.1941548809452595E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4525585960458567E+0;
    b=0.1751165438438091E+0;
    v=0.1980078427252384E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4848447779622947E+0;
    b=0.2056338306745660E+0;
    v=0.2011296284744488E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5160871208276894E+0;
    b=0.2359965487229226E+0;
    v=0.2035888456966776E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5462112185696926E+0;
    b=0.2660430223139146E+0;
    v=0.2054516325352142E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5751425068101757E+0;
    b=0.2956193664498032E+0;
    v=0.2067831033092635E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6028073872853596E+0;
    b=0.3245763905312779E+0;
    v=0.2076485320284876E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6291338275278409E+0;
    b=0.3527670026206972E+0;
    v=0.2081141439525255E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3541797528439391E+0;
    b=0.2823853479435550E-1;
    v=0.1834383015469222E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3908234972074657E+0;
    b=0.5741296374713106E-1;
    v=0.1889540591777677E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4264408450107590E+0;
    b=0.8724646633650199E-1;
    v=0.1936677023597375E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4609949666553286E+0;
    b=0.1175034422915616E+0;
    v=0.1976176495066504E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4944389496536006E+0;
    b=0.1479755652628428E+0;
    v=0.2008536004560983E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5267194884346086E+0;
    b=0.1784740659484352E+0;
    v=0.2034280351712291E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5577787810220990E+0;
    b=0.2088245700431244E+0;
    v=0.2053944466027758E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5875563763536670E+0;
    b=0.2388628136570763E+0;
    v=0.2068077642882360E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6159910016391269E+0;
    b=0.2684308928769185E+0;
    v=0.2077250949661599E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6430219602956268E+0;
    b=0.2973740761960252E+0;
    v=0.2082062440705320E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4300647036213646E+0;
    b=0.2916399920493977E-1;
    v=0.1934374486546626E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4661486308935531E+0;
    b=0.5898803024755659E-1;
    v=0.1974107010484300E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5009658555287261E+0;
    b=0.8924162698525409E-1;
    v=0.2007129290388658E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5344824270447704E+0;
    b=0.1197185199637321E+0;
    v=0.2033736947471293E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5666575997416371E+0;
    b=0.1502300756161382E+0;
    v=0.2054287125902493E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5974457471404752E+0;
    b=0.1806004191913564E+0;
    v=0.2069184936818894E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6267984444116886E+0;
    b=0.2106621764786252E+0;
    v=0.2078883689808782E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6546664713575417E+0;
    b=0.2402526932671914E+0;
    v=0.2083886366116359E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5042711004437253E+0;
    b=0.2982529203607657E-1;
    v=0.2006593275470817E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5392127456774380E+0;
    b=0.6008728062339922E-1;
    v=0.2033728426135397E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5726819437668618E+0;
    b=0.9058227674571398E-1;
    v=0.2055008781377608E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6046469254207278E+0;
    b=0.1211219235803400E+0;
    v=0.2070651783518502E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6350716157434952E+0;
    b=0.1515286404791580E+0;
    v=0.2080953335094320E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6639177679185454E+0;
    b=0.1816314681255552E+0;
    v=0.2086284998988521E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5757276040972253E+0;
    b=0.3026991752575440E-1;
    v=0.2055549387644668E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6090265823139755E+0;
    b=0.6078402297870770E-1;
    v=0.2071871850267654E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6406735344387661E+0;
    b=0.9135459984176636E-1;
    v=0.2082856600431965E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6706397927793709E+0;
    b=0.1218024155966590E+0;
    v=0.2088705858819358E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6435019674426665E+0;
    b=0.3052608357660639E-1;
    v=0.2083995867536322E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6747218676375681E+0;
    b=0.6112185773983089E-1;
    v=0.2090509712889637E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;

    case 5810:

    v=0.9735347946175486E-5;
    start = lebedevReccurence(1,start,a,b,v,s);
    v=0.1907581241803167E-3;
    start = lebedevReccurence(2,start,a,b,v,s);
    v=0.1901059546737578E-3;
    start = lebedevReccurence(3,start,a,b,v,s);
    a=0.1182361662400277E-1;
    v=0.3926424538919212E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3062145009138958E-1;
    v=0.6667905467294382E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5329794036834243E-1;
    v=0.8868891315019135E-4;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7848165532862220E-1;
    v=0.1066306000958872E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1054038157636201E+0;
    v=0.1214506743336128E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1335577797766211E+0;
    v=0.1338054681640871E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1625769955502252E+0;
    v=0.1441677023628504E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.1921787193412792E+0;
    v=0.1528880200826557E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2221340534690548E+0;
    v=0.1602330623773609E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2522504912791132E+0;
    v=0.1664102653445244E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.2823610860679697E+0;
    v=0.1715845854011323E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3123173966267560E+0;
    v=0.1758901000133069E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3419847036953789E+0;
    v=0.1794382485256736E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3712386456999758E+0;
    v=0.1823238106757407E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3999627649876828E+0;
    v=0.1846293252959976E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4280466458648093E+0;
    v=0.1864284079323098E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4553844360185711E+0;
    v=0.1877882694626914E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.4818736094437834E+0;
    v=0.1887716321852025E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5074138709260629E+0;
    v=0.1894381638175673E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5319061304570707E+0;
    v=0.1898454899533629E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5552514978677286E+0;
    v=0.1900497929577815E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.5981009025246183E+0;
    v=0.1900671501924092E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6173990192228116E+0;
    v=0.1899837555533510E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6351365239411131E+0;
    v=0.1899014113156229E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6512010228227200E+0;
    v=0.1898581257705106E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6654758363948120E+0;
    v=0.1898804756095753E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6778410414853370E+0;
    v=0.1899793610426402E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6881760887484110E+0;
    v=0.1901464554844117E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.6963645267094598E+0;
    v=0.1903533246259542E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7023010617153579E+0;
    v=0.1905556158463228E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.7059004636628753E+0;
    v=0.1907037155663528E-3;
    start = lebedevReccurence(4,start,a,b,v,s);
    a=0.3552470312472575E-1;
    v=0.5992997844249967E-4;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.9151176620841283E-1;
    v=0.9749059382456978E-4;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.1566197930068980E+0;
    v=0.1241680804599158E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2265467599271907E+0;
    v=0.1437626154299360E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.2988242318581361E+0;
    v=0.1584200054793902E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.3717482419703886E+0;
    v=0.1694436550982744E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.4440094491758889E+0;
    v=0.1776617014018108E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5145337096756642E+0;
    v=0.1836132434440077E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.5824053672860230E+0;
    v=0.1876494727075983E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6468283961043370E+0;
    v=0.1899906535336482E-3;
    start = lebedevReccurence(5,start,a,b,v,s);
    a=0.6095964259104373E-1;
    b=0.1787828275342931E-1;
    v=0.8143252820767350E-4;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.8811962270959388E-1;
    b=0.3953888740792096E-1;
    v=0.9998859890887728E-4;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1165936722428831E+0;
    b=0.6378121797722990E-1;
    v=0.1156199403068359E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1460232857031785E+0;
    b=0.8985890813745037E-1;
    v=0.1287632092635513E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1761197110181755E+0;
    b=0.1172606510576162E+0;
    v=0.1398378643365139E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2066471190463718E+0;
    b=0.1456102876970995E+0;
    v=0.1491876468417391E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2374076026328152E+0;
    b=0.1746153823011775E+0;
    v=0.1570855679175456E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2682305474337051E+0;
    b=0.2040383070295584E+0;
    v=0.1637483948103775E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2989653312142369E+0;
    b=0.2336788634003698E+0;
    v=0.1693500566632843E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3294762752772209E+0;
    b=0.2633632752654219E+0;
    v=0.1740322769393633E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3596390887276086E+0;
    b=0.2929369098051601E+0;
    v=0.1779126637278296E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3893383046398812E+0;
    b=0.3222592785275512E+0;
    v=0.1810908108835412E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4184653789358347E+0;
    b=0.3512004791195743E+0;
    v=0.1836529132600190E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4469172319076166E+0;
    b=0.3796385677684537E+0;
    v=0.1856752841777379E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4745950813276976E+0;
    b=0.4074575378263879E+0;
    v=0.1872270566606832E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5014034601410262E+0;
    b=0.4345456906027828E+0;
    v=0.1883722645591307E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5272493404551239E+0;
    b=0.4607942515205134E+0;
    v=0.1891714324525297E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5520413051846366E+0;
    b=0.4860961284181720E+0;
    v=0.1896827480450146E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5756887237503077E+0;
    b=0.5103447395342790E+0;
    v=0.1899628417059528E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1225039430588352E+0;
    b=0.2136455922655793E-1;
    v=0.1123301829001669E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1539113217321372E+0;
    b=0.4520926166137188E-1;
    v=0.1253698826711277E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1856213098637712E+0;
    b=0.7086468177864818E-1;
    v=0.1366266117678531E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2174998728035131E+0;
    b=0.9785239488772918E-1;
    v=0.1462736856106918E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2494128336938330E+0;
    b=0.1258106396267210E+0;
    v=0.1545076466685412E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2812321562143480E+0;
    b=0.1544529125047001E+0;
    v=0.1615096280814007E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3128372276456111E+0;
    b=0.1835433512202753E+0;
    v=0.1674366639741759E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3441145160177973E+0;
    b=0.2128813258619585E+0;
    v=0.1724225002437900E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3749567714853510E+0;
    b=0.2422913734880829E+0;
    v=0.1765810822987288E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4052621732015610E+0;
    b=0.2716163748391453E+0;
    v=0.1800104126010751E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4349335453522385E+0;
    b=0.3007127671240280E+0;
    v=0.1827960437331284E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4638776641524965E+0;
    b=0.3294470677216479E+0;
    v=0.1850140300716308E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4920046410462687E+0;
    b=0.3576932543699155E+0;
    v=0.1867333507394938E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5192273554861704E+0;
    b=0.3853307059757764E+0;
    v=0.1880178688638289E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5454609081136522E+0;
    b=0.4122425044452694E+0;
    v=0.1889278925654758E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5706220661424140E+0;
    b=0.4383139587781027E+0;
    v=0.1895213832507346E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5946286755181518E+0;
    b=0.4634312536300553E+0;
    v=0.1898548277397420E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.1905370790924295E+0;
    b=0.2371311537781979E-1;
    v=0.1349105935937341E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2242518717748009E+0;
    b=0.4917878059254806E-1;
    v=0.1444060068369326E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2577190808025936E+0;
    b=0.7595498960495142E-1;
    v=0.1526797390930008E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2908724534927187E+0;
    b=0.1036991083191100E+0;
    v=0.1598208771406474E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3236354020056219E+0;
    b=0.1321348584450234E+0;
    v=0.1659354368615331E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3559267359304543E+0;
    b=0.1610316571314789E+0;
    v=0.1711279910946440E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3876637123676956E+0;
    b=0.1901912080395707E+0;
    v=0.1754952725601440E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4187636705218842E+0;
    b=0.2194384950137950E+0;
    v=0.1791247850802529E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4491449019883107E+0;
    b=0.2486155334763858E+0;
    v=0.1820954300877716E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4787270932425445E+0;
    b=0.2775768931812335E+0;
    v=0.1844788524548449E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5074315153055574E+0;
    b=0.3061863786591120E+0;
    v=0.1863409481706220E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5351810507738336E+0;
    b=0.3343144718152556E+0;
    v=0.1877433008795068E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5619001025975381E+0;
    b=0.3618362729028427E+0;
    v=0.1887444543705232E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5875144035268046E+0;
    b=0.3886297583620408E+0;
    v=0.1894009829375006E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6119507308734495E+0;
    b=0.4145742277792031E+0;
    v=0.1897683345035198E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2619733870119463E+0;
    b=0.2540047186389353E-1;
    v=0.1517327037467653E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.2968149743237949E+0;
    b=0.5208107018543989E-1;
    v=0.1587740557483543E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3310451504860488E+0;
    b=0.7971828470885599E-1;
    v=0.1649093382274097E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3646215567376676E+0;
    b=0.1080465999177927E+0;
    v=0.1701915216193265E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3974916785279360E+0;
    b=0.1368413849366629E+0;
    v=0.1746847753144065E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4295967403772029E+0;
    b=0.1659073184763559E+0;
    v=0.1784555512007570E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4608742854473447E+0;
    b=0.1950703730454614E+0;
    v=0.1815687562112174E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4912598858949903E+0;
    b=0.2241721144376724E+0;
    v=0.1840864370663302E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5206882758945558E+0;
    b=0.2530655255406489E+0;
    v=0.1860676785390006E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5490940914019819E+0;
    b=0.2816118409731066E+0;
    v=0.1875690583743703E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5764123302025542E+0;
    b=0.3096780504593238E+0;
    v=0.1886453236347225E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6025786004213506E+0;
    b=0.3371348366394987E+0;
    v=0.1893501123329645E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6275291964794956E+0;
    b=0.3638547827694396E+0;
    v=0.1897366184519868E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3348189479861771E+0;
    b=0.2664841935537443E-1;
    v=0.1643908815152736E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.3699515545855295E+0;
    b=0.5424000066843495E-1;
    v=0.1696300350907768E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4042003071474669E+0;
    b=0.8251992715430854E-1;
    v=0.1741553103844483E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4375320100182624E+0;
    b=0.1112695182483710E+0;
    v=0.1780015282386092E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4699054490335947E+0;
    b=0.1402964116467816E+0;
    v=0.1812116787077125E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5012739879431952E+0;
    b=0.1694275117584291E+0;
    v=0.1838323158085421E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5315874883754966E+0;
    b=0.1985038235312689E+0;
    v=0.1859113119837737E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5607937109622117E+0;
    b=0.2273765660020893E+0;
    v=0.1874969220221698E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5888393223495521E+0;
    b=0.2559041492849764E+0;
    v=0.1886375612681076E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6156705979160163E+0;
    b=0.2839497251976899E+0;
    v=0.1893819575809276E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6412338809078123E+0;
    b=0.3113791060500690E+0;
    v=0.1897794748256767E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4076051259257167E+0;
    b=0.2757792290858463E-1;
    v=0.1738963926584846E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4423788125791520E+0;
    b=0.5584136834984293E-1;
    v=0.1777442359873466E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4760480917328258E+0;
    b=0.8457772087727143E-1;
    v=0.1810010815068719E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5085838725946297E+0;
    b=0.1135975846359248E+0;
    v=0.1836920318248129E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5399513637391218E+0;
    b=0.1427286904765053E+0;
    v=0.1858489473214328E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5701118433636380E+0;
    b=0.1718112740057635E+0;
    v=0.1875079342496592E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5990240530606021E+0;
    b=0.2006944855985351E+0;
    v=0.1887080239102310E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6266452685139695E+0;
    b=0.2292335090598907E+0;
    v=0.1894905752176822E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6529320971415942E+0;
    b=0.2572871512353714E+0;
    v=0.1898991061200695E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.4791583834610126E+0;
    b=0.2826094197735932E-1;
    v=0.1809065016458791E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5130373952796940E+0;
    b=0.5699871359683649E-1;
    v=0.1836297121596799E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5456252429628476E+0;
    b=0.8602712528554394E-1;
    v=0.1858426916241869E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5768956329682385E+0;
    b=0.1151748137221281E+0;
    v=0.1875654101134641E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6068186944699046E+0;
    b=0.1442811654136362E+0;
    v=0.1888240751833503E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6353622248024907E+0;
    b=0.1731930321657680E+0;
    v=0.1896497383866979E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6624927035731797E+0;
    b=0.2017619958756061E+0;
    v=0.1900775530219121E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5484933508028488E+0;
    b=0.2874219755907391E-1;
    v=0.1858525041478814E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.5810207682142106E+0;
    b=0.5778312123713695E-1;
    v=0.1876248690077947E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6120955197181352E+0;
    b=0.8695262371439526E-1;
    v=0.1889404439064607E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6416944284294319E+0;
    b=0.1160893767057166E+0;
    v=0.1898168539265290E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6697926391731260E+0;
    b=0.1450378826743251E+0;
    v=0.1902779940661772E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6147594390585488E+0;
    b=0.2904957622341456E-1;
    v=0.1890125641731815E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6455390026356783E+0;
    b=0.5823809152617197E-1;
    v=0.1899434637795751E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6747258588365477E+0;
    b=0.8740384899884715E-1;
    v=0.1904520856831751E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    a=0.6772135750395347E+0;
    b=0.2919946135808105E-1;
    v=0.1905534498734563E-3;
    start = lebedevReccurence(6,start,a,b,v,s);
    break;
        
    }
    return boost::shared_ptr<SphericalGrid>(s);
}

int SphericalGrid::lebedevReccurence(int type, int start, double a, double b, double v, SphericalGrid* leb)
{

    int end;
    double c;

    switch (type){

    case 1:
    a = 1.0;

    leb->x_[start] = a;
    leb->y_[start] = 0.0;
    leb->z_[start] = 0.0;
    leb->w_[start] = 4.0*M_PI*v;

    leb->x_[start+1] = -a;
    leb->y_[start+1] = 0.0;
    leb->z_[start+1] = 0.0;
    leb->w_[start+1] = 4.0*M_PI*v;

    leb->x_[start+2] = 0.0;
    leb->y_[start+2] = a;
    leb->z_[start+2] = 0.0;
    leb->w_[start+2] = 4.0*M_PI*v;

    leb->x_[start+3] = 0.0;
    leb->y_[start+3] = -a;
    leb->z_[start+3] = 0.0;
    leb->w_[start+3] = 4.0*M_PI*v;

    leb->x_[start+4] = 0.0;
    leb->y_[start+4] = 0.0;
    leb->z_[start+4] = a;
    leb->w_[start+4] = 4.0*M_PI*v;

    leb->x_[start+5] = 0.0;
    leb->y_[start+5] = 0.0;
    leb->z_[start+5] = -a;
    leb->w_[start+5] = 4.0*M_PI*v;
    end = start+6;
    break;

    case 2:
    a = sqrt(0.5);
    leb->x_[start] = 0.0;
    leb->y_[start] = a;
    leb->z_[start] = a;
    leb->w_[start] = 4.0*M_PI*v;

    leb->x_[start+1] = 0.0;
    leb->y_[start+1] = -a;
    leb->z_[start+1] = a;
    leb->w_[start+1] = 4.0*M_PI*v;

    leb->x_[start+2] = 0.0;
    leb->y_[start+2] = a;
    leb->z_[start+2] = -a;
    leb->w_[start+2] = 4.0*M_PI*v;

    leb->x_[start+3] = 0.0;
    leb->y_[start+3] = -a;
    leb->z_[start+3] = -a;
    leb->w_[start+3] = 4.0*M_PI*v;

    leb->x_[start+4] = a;
    leb->y_[start+4] = 0.0;
    leb->z_[start+4] = a;
    leb->w_[start+4] = 4.0*M_PI*v;

    leb->x_[start+5] = a;
    leb->y_[start+5] = 0.0;
    leb->z_[start+5] = -a;
    leb->w_[start+5] = 4.0*M_PI*v;

    leb->x_[start+6] = -a;
    leb->y_[start+6] = 0.0;
    leb->z_[start+6] = a;
    leb->w_[start+6] = 4.0*M_PI*v;

    leb->x_[start+7] = -a;
    leb->y_[start+7] = 0.0;
    leb->z_[start+7] = -a;
    leb->w_[start+7] = 4.0*M_PI*v;

    leb->x_[start+8] = a;
    leb->y_[start+8] = a;
    leb->z_[start+8] = 0.0;
    leb->w_[start+8] = 4.0*M_PI*v;

    leb->x_[start+9] = -a;
    leb->y_[start+9] = a;
    leb->z_[start+9] = 0.0;
    leb->w_[start+9] = 4.0*M_PI*v;

    leb->x_[start+10] = a;
    leb->y_[start+10] = -a;
    leb->z_[start+10] = 0.0;
    leb->w_[start+10] = 4.0*M_PI*v;

    leb->x_[start+11] = -a;
    leb->y_[start+11] = -a;
    leb->z_[start+11] = 0.0;
    leb->w_[start+11] = 4.0*M_PI*v;
    end = start+12;
    break;

    case 3:
    a = sqrt(1.0/3.0);
    leb->x_[start] = a;
    leb->y_[start] = a;
    leb->z_[start] = a;
    leb->w_[start] = 4.0*M_PI*v;

    leb->x_[start+1] = -a;
    leb->y_[start+1] = a;
    leb->z_[start+1] = a;
    leb->w_[start+1] = 4.0*M_PI*v;

    leb->x_[start+2] = a;
    leb->y_[start+2] = -a;
    leb->z_[start+2] = a;
    leb->w_[start+2] = 4.0*M_PI*v;

    leb->x_[start+3] = a;
    leb->y_[start+3] = a;
    leb->z_[start+3] = -a;
    leb->w_[start+3] = 4.0*M_PI*v;

    leb->x_[start+4] = -a;
    leb->y_[start+4] = -a;
    leb->z_[start+4] = a;
    leb->w_[start+4] = 4.0*M_PI*v;

    leb->x_[start+5] = a;
    leb->y_[start+5] = -a;
    leb->z_[start+5] = -a;
    leb->w_[start+5] = 4.0*M_PI*v;

    leb->x_[start+6] = -a;
    leb->y_[start+6] = a;
    leb->z_[start+6] = -a;
    leb->w_[start+6] = 4.0*M_PI*v;

    leb->x_[start+7] = -a;
    leb->y_[start+7] = -a;
    leb->z_[start+7] = -a;
    leb->w_[start+7] = 4.0*M_PI*v;
    end = start+8;
    break;

    case 4:
    b = sqrt(1.0 - 2.0*a*a);
    leb->x_[start] = a;
    leb->y_[start] = a;
    leb->z_[start] = b;
    leb->w_[start] = 4.0*M_PI*v;

    leb->x_[start+1] = -a;
    leb->y_[start+1] = a;
    leb->z_[start+1] = b;
    leb->w_[start+1] = 4.0*M_PI*v;

    leb->x_[start+2] = a;
    leb->y_[start+2] = -a;
    leb->z_[start+2] = b;
    leb->w_[start+2] = 4.0*M_PI*v;

    leb->x_[start+3] = a;
    leb->y_[start+3] = a;
    leb->z_[start+3] = -b;
    leb->w_[start+3] = 4.0*M_PI*v;

    leb->x_[start+4] = -a;
    leb->y_[start+4] = -a;
    leb->z_[start+4] = b;
    leb->w_[start+4] = 4.0*M_PI*v;

    leb->x_[start+5] = -a;
    leb->y_[start+5] = a;
    leb->z_[start+5] = -b;
    leb->w_[start+5] = 4.0*M_PI*v;

    leb->x_[start+6] = a;
    leb->y_[start+6] = -a;
    leb->z_[start+6] = -b;
    leb->w_[start+6] = 4.0*M_PI*v;

    leb->x_[start+7] = -a;
    leb->y_[start+7] = -a;
    leb->z_[start+7] = -b;
    leb->w_[start+7] = 4.0*M_PI*v;

    leb->x_[start+8] = -a;
    leb->y_[start+8] = b;
    leb->z_[start+8] = a;
    leb->w_[start+8] = 4.0*M_PI*v;

    leb->x_[start+9] = a;
    leb->y_[start+9] = -b;
    leb->z_[start+9] = a;
    leb->w_[start+9] = 4.0*M_PI*v;

    leb->x_[start+10] = a;
    leb->y_[start+10] = b;
    leb->z_[start+10] = -a;
    leb->w_[start+10] = 4.0*M_PI*v;

    leb->x_[start+11] = -a;
    leb->y_[start+11] = -b;
    leb->z_[start+11] = a;
    leb->w_[start+11] = 4.0*M_PI*v;

    leb->x_[start+12] = -a;
    leb->y_[start+12] = b;
    leb->z_[start+12] = -a;
    leb->w_[start+12] = 4.0*M_PI*v;

    leb->x_[start+13] = a;
    leb->y_[start+13] = -b;
    leb->z_[start+13] = -a;
    leb->w_[start+13] = 4.0*M_PI*v;

    leb->x_[start+14] = -a;
    leb->y_[start+14] = -b;
    leb->z_[start+14] = -a;
    leb->w_[start+14] = 4.0*M_PI*v;

    leb->x_[start+15] = a;
    leb->y_[start+15] = b;
    leb->z_[start+15] = a;
    leb->w_[start+15] = 4.0*M_PI*v;

    leb->x_[start+16] = b;
    leb->y_[start+16] = a;
    leb->z_[start+16] = a;
    leb->w_[start+16] = 4.0*M_PI*v;

    leb->x_[start+17] = -b;
    leb->y_[start+17] = a;
    leb->z_[start+17] = a;
    leb->w_[start+17] = 4.0*M_PI*v;

    leb->x_[start+18] = b;
    leb->y_[start+18] = -a;
    leb->z_[start+18] = a;
    leb->w_[start+18] = 4.0*M_PI*v;

    leb->x_[start+19] = b;
    leb->y_[start+19] = a;
    leb->z_[start+19] = -a;
    leb->w_[start+19] = 4.0*M_PI*v;

    leb->x_[start+20] = -b;
    leb->y_[start+20] = -a;
    leb->z_[start+20] = a;
    leb->w_[start+20] = 4.0*M_PI*v;

    leb->x_[start+21] = -b;
    leb->y_[start+21] = a;
    leb->z_[start+21] = -a;
    leb->w_[start+21] = 4.0*M_PI*v;

    leb->x_[start+22] = b;
    leb->y_[start+22] = -a;
    leb->z_[start+22] = -a;
    leb->w_[start+22] = 4.0*M_PI*v;

    leb->x_[start+23] = -b;
    leb->y_[start+23] = -a;
    leb->z_[start+23] = -a;
    leb->w_[start+23] = 4.0*M_PI*v;
    end = start + 24;
    break;

    case 5:
    b=sqrt(1-a*a);
    leb->x_[start] = a;
    leb->y_[start] = b;
    leb->z_[start] = 0.0;
    leb->w_[start] = 4.0*M_PI*v;

    leb->x_[start+1] = -a;
    leb->y_[start+1] = b;
    leb->z_[start+1] = 0.0;
    leb->w_[start+1] = 4.0*M_PI*v;

    leb->x_[start+2] = a;
    leb->y_[start+2] = -b;
    leb->z_[start+2] = 0.0;
    leb->w_[start+2] = 4.0*M_PI*v;

    leb->x_[start+3] = -a;
    leb->y_[start+3] = -b;
    leb->z_[start+3] = 0.0;
    leb->w_[start+3] = 4.0*M_PI*v;

    leb->x_[start+4] = b;
    leb->y_[start+4] = a;
    leb->z_[start+4] = 0.0;
    leb->w_[start+4] = 4.0*M_PI*v;

    leb->x_[start+5] = -b;
    leb->y_[start+5] = a;
    leb->z_[start+5] = 0.0;
    leb->w_[start+5] = 4.0*M_PI*v;

    leb->x_[start+6] = b;
    leb->y_[start+6] = -a;
    leb->z_[start+6] = 0.0;
    leb->w_[start+6] = 4.0*M_PI*v;

    leb->x_[start+7] = -b;
    leb->y_[start+7] = -a;
    leb->z_[start+7] = 0.0;
    leb->w_[start+7] = 4.0*M_PI*v;

    leb->x_[start+8] = a;
    leb->y_[start+8] = 0.0;
    leb->z_[start+8] = b;
    leb->w_[start+8] = 4.0*M_PI*v;

    leb->x_[start+9] = -a;
    leb->y_[start+9] = 0.0;
    leb->z_[start+9] = b;
    leb->w_[start+9] = 4.0*M_PI*v;

    leb->x_[start+10] = a;
    leb->y_[start+10] = 0.0;
    leb->z_[start+10] = -b;
    leb->w_[start+10] = 4.0*M_PI*v;

    leb->x_[start+11] = -a;
    leb->y_[start+11] = 0.0;
    leb->z_[start+11] = -b;
    leb->w_[start+11] = 4.0*M_PI*v;

    leb->x_[start+12] = b;
    leb->y_[start+12] = 0.0;
    leb->z_[start+12] = a;
    leb->w_[start+12] = 4.0*M_PI*v;

    leb->x_[start+13] = -b;
    leb->y_[start+13] = 0.0;
    leb->z_[start+13] = a;
    leb->w_[start+13] = 4.0*M_PI*v;

    leb->x_[start+14] = b;
    leb->y_[start+14] = 0.0;
    leb->z_[start+14] = -a;
    leb->w_[start+14] = 4.0*M_PI*v;

    leb->x_[start+15] = -b;
    leb->y_[start+15] = 0.0;
    leb->z_[start+15] = -a;
    leb->w_[start+15] = 4.0*M_PI*v;

    leb->x_[start+16] = 0.0;
    leb->y_[start+16] = a;
    leb->z_[start+16] = b;
    leb->w_[start+16] = 4.0*M_PI*v;

    leb->x_[start+17] = 0.0;
    leb->y_[start+17] = -a;
    leb->z_[start+17] = b;
    leb->w_[start+17] = 4.0*M_PI*v;

    leb->x_[start+18] = 0.0;
    leb->y_[start+18] = a;
    leb->z_[start+18] = -b;
    leb->w_[start+18] = 4.0*M_PI*v;

    leb->x_[start+19] = 0.0;
    leb->y_[start+19] = -a;
    leb->z_[start+19] = -b;
    leb->w_[start+19] = 4.0*M_PI*v;

    leb->x_[start+20] = 0.0;
    leb->y_[start+20] = b;
    leb->z_[start+20] = a;
    leb->w_[start+20] = 4.0*M_PI*v;

    leb->x_[start+21] = 0.0;
    leb->y_[start+21] = -b;
    leb->z_[start+21] = a;
    leb->w_[start+21] = 4.0*M_PI*v;

    leb->x_[start+22] = 0.0;
    leb->y_[start+22] = b;
    leb->z_[start+22] = -a;
    leb->w_[start+22] = 4.0*M_PI*v;

    leb->x_[start+23] = 0.0;
    leb->y_[start+23] = -b;
    leb->z_[start+23] = -a;
    leb->w_[start+23] = 4.0*M_PI*v;
    end = start + 24;
    break;

    case 6:
    c=sqrt(1.0 - a*a - b*b);
    leb->x_[start] = a;
    leb->y_[start] = b;
    leb->z_[start] = c;
    leb->w_[start] = 4.0*M_PI*v;

    leb->x_[start+1] = -a;
    leb->y_[start+1] = b;
    leb->z_[start+1] = c;
    leb->w_[start+1] = 4.0*M_PI*v;

    leb->x_[start+2] = a;
    leb->y_[start+2] = -b;
    leb->z_[start+2] = c;
    leb->w_[start+2] = 4.0*M_PI*v;

    leb->x_[start+3] = a;
    leb->y_[start+3] = b;
    leb->z_[start+3] = -c;
    leb->w_[start+3] = 4.0*M_PI*v;

    leb->x_[start+4] = -a;
    leb->y_[start+4] = -b;
    leb->z_[start+4] = c;
    leb->w_[start+4] = 4.0*M_PI*v;

    leb->x_[start+5] = a;
    leb->y_[start+5] = -b;
    leb->z_[start+5] = -c;
    leb->w_[start+5] = 4.0*M_PI*v;

    leb->x_[start+6] = -a;
    leb->y_[start+6] = b;
    leb->z_[start+6] = -c;
    leb->w_[start+6] = 4.0*M_PI*v;

    leb->x_[start+7] = -a;
    leb->y_[start+7] = -b;
    leb->z_[start+7] = -c;
    leb->w_[start+7] = 4.0*M_PI*v;

    leb->x_[start+8] = b;
    leb->y_[start+8] = a;
    leb->z_[start+8] = c;
    leb->w_[start+8] = 4.0*M_PI*v;

    leb->x_[start+9] = -b;
    leb->y_[start+9] = a;
    leb->z_[start+9] = c;
    leb->w_[start+9] = 4.0*M_PI*v;

    leb->x_[start+10] = b;
    leb->y_[start+10] = -a;
    leb->z_[start+10] = c;
    leb->w_[start+10] = 4.0*M_PI*v;

    leb->x_[start+11] = b;
    leb->y_[start+11] = a;
    leb->z_[start+11] = -c;
    leb->w_[start+11] = 4.0*M_PI*v;

    leb->x_[start+12] = -b;
    leb->y_[start+12] = -a;
    leb->z_[start+12] = c;
    leb->w_[start+12] = 4.0*M_PI*v;

    leb->x_[start+13] = b;
    leb->y_[start+13] = -a;
    leb->z_[start+13] = -c;
    leb->w_[start+13] = 4.0*M_PI*v;

    leb->x_[start+14] = -b;
    leb->y_[start+14] = a;
    leb->z_[start+14] = -c;
    leb->w_[start+14] = 4.0*M_PI*v;

    leb->x_[start+15] = -b;
    leb->y_[start+15] = -a;
    leb->z_[start+15] = -c;
    leb->w_[start+15] = 4.0*M_PI*v;

    leb->x_[start+16] = c;
    leb->y_[start+16] = a;
    leb->z_[start+16] = b;
    leb->w_[start+16] = 4.0*M_PI*v;

    leb->x_[start+17] = -c;
    leb->y_[start+17] = a;
    leb->z_[start+17] = b;
    leb->w_[start+17] = 4.0*M_PI*v;

    leb->x_[start+18] = c;
    leb->y_[start+18] = -a;
    leb->z_[start+18] = b;
    leb->w_[start+18] = 4.0*M_PI*v;

    leb->x_[start+19] = c;
    leb->y_[start+19] = a;
    leb->z_[start+19] = -b;
    leb->w_[start+19] = 4.0*M_PI*v;

    leb->x_[start+20] = -c;
    leb->y_[start+20] = -a;
    leb->z_[start+20] = b;
    leb->w_[start+20] = 4.0*M_PI*v;

    leb->x_[start+21] = c;
    leb->y_[start+21] = -a;
    leb->z_[start+21] = -b;
    leb->w_[start+21] = 4.0*M_PI*v;

    leb->x_[start+22] = -c;
    leb->y_[start+22] = a;
    leb->z_[start+22] = -b;
    leb->w_[start+22] = 4.0*M_PI*v;

    leb->x_[start+23] = -c;
    leb->y_[start+23] = -a;
    leb->z_[start+23] = -b;
    leb->w_[start+23] = 4.0*M_PI*v;

    leb->x_[start+24] = c;
    leb->y_[start+24] = b;
    leb->z_[start+24] = a;
    leb->w_[start+24] = 4.0*M_PI*v;

    leb->x_[start+25] = -c;
    leb->y_[start+25] = b;
    leb->z_[start+25] = a;
    leb->w_[start+25] = 4.0*M_PI*v;

    leb->x_[start+26] = c;
    leb->y_[start+26] = -b;
    leb->z_[start+26] = a;
    leb->w_[start+26] = 4.0*M_PI*v;

    leb->x_[start+27] = c;
    leb->y_[start+27] = b;
    leb->z_[start+27] = -a;
    leb->w_[start+27] = 4.0*M_PI*v;

    leb->x_[start+28] = -c;
    leb->y_[start+28] = -b;
    leb->z_[start+28] = a;
    leb->w_[start+28] = 4.0*M_PI*v;

    leb->x_[start+29] = c;
    leb->y_[start+29] = -b;
    leb->z_[start+29] = -a;
    leb->w_[start+29] = 4.0*M_PI*v;

    leb->x_[start+30] = -c;
    leb->y_[start+30] = b;
    leb->z_[start+30] = -a;
    leb->w_[start+30] = 4.0*M_PI*v;

    leb->x_[start+31] = -c;
    leb->y_[start+31] = -b;
    leb->z_[start+31] = -a;
    leb->w_[start+31] = 4.0*M_PI*v;

    leb->x_[start+32] = a;
    leb->y_[start+32] = c;
    leb->z_[start+32] = b;
    leb->w_[start+32] = 4.0*M_PI*v;

    leb->x_[start+33] = -a;
    leb->y_[start+33] = c;
    leb->z_[start+33] = b;
    leb->w_[start+33] = 4.0*M_PI*v;

    leb->x_[start+34] = a;
    leb->y_[start+34] = -c;
    leb->z_[start+34] = b;
    leb->w_[start+34] = 4.0*M_PI*v;

    leb->x_[start+35] = a;
    leb->y_[start+35] = c;
    leb->z_[start+35] = -b;
    leb->w_[start+35] = 4.0*M_PI*v;

    leb->x_[start+36] = -a;
    leb->y_[start+36] = -c;
    leb->z_[start+36] = b;
    leb->w_[start+36] = 4.0*M_PI*v;

    leb->x_[start+37] = a;
    leb->y_[start+37] = -c;
    leb->z_[start+37] = -b;
    leb->w_[start+37] = 4.0*M_PI*v;

    leb->x_[start+38] = -a;
    leb->y_[start+38] = c;
    leb->z_[start+38] = -b;
    leb->w_[start+38] = 4.0*M_PI*v;

    leb->x_[start+39] = -a;
    leb->y_[start+39] = -c;
    leb->z_[start+39] = -b;
    leb->w_[start+39] = 4.0*M_PI*v;

    leb->x_[start+40] = b;
    leb->y_[start+40] = c;
    leb->z_[start+40] = a;
    leb->w_[start+40] = 4.0*M_PI*v;

    leb->x_[start+41] = -b;
    leb->y_[start+41] = c;
    leb->z_[start+41] = a;
    leb->w_[start+41] = 4.0*M_PI*v;

    leb->x_[start+42] = b;
    leb->y_[start+42] = -c;
    leb->z_[start+42] = a;
    leb->w_[start+42] = 4.0*M_PI*v;

    leb->x_[start+43] = b;
    leb->y_[start+43] = c;
    leb->z_[start+43] = -a;
    leb->w_[start+43] = 4.0*M_PI*v;

    leb->x_[start+44] = -b;
    leb->y_[start+44] = -c;
    leb->z_[start+44] = a;
    leb->w_[start+44] = 4.0*M_PI*v;

    leb->x_[start+45] = b;
    leb->y_[start+45] = -c;
    leb->z_[start+45] = -a;
    leb->w_[start+45] = 4.0*M_PI*v;

    leb->x_[start+46] = -b;
    leb->y_[start+46] = c;
    leb->z_[start+46] = -a;
    leb->w_[start+46] = 4.0*M_PI*v;

    leb->x_[start+47] = -b;
    leb->y_[start+47] = -c;
    leb->z_[start+47] = -a;
    leb->w_[start+47] = 4.0*M_PI*v;
    end = start + 48;
    break;

    default:
    throw std::domain_error("\n Incorrect Reccurence order (should be 1,2,3,4,5,or 6)");
    }
    return end;
}

GridBlocker::GridBlocker(const int npoints_ref, double const* x_ref, double const* y_ref, double const* z_ref,
        double const* w_ref, const int max_points, const int min_points, boost::shared_ptr<BasisExtents> extents) :
    npoints_ref_(npoints_ref), x_ref_(x_ref), y_ref_(y_ref), z_ref_(z_ref), w_ref_(w_ref), 
    tol_max_points_(max_points), tol_min_points_(min_points), extents_(extents), print_(1), debug_(0)
{
}
GridBlocker::~GridBlocker()
{
}
NaiveGridBlocker::NaiveGridBlocker(const int npoints_ref, double const* x_ref, double const* y_ref, double const* z_ref,
        double const* w_ref, const int max_points, const int min_points, boost::shared_ptr<BasisExtents> extents) :
    GridBlocker(npoints_ref,x_ref,y_ref,z_ref,w_ref,max_points,min_points,extents)
{
}
NaiveGridBlocker::~NaiveGridBlocker()
{
}
void NaiveGridBlocker::block()
{
    npoints_ = npoints_ref_;
    max_points_ = tol_max_points_;
    max_functions_ = extents_->basis()->nbf();

    x_ = new double[npoints_]; 
    y_ = new double[npoints_]; 
    z_ = new double[npoints_]; 
    w_ = new double[npoints_]; 

    ::memcpy((void*)x_,(void*)x_ref_, sizeof(double)*npoints_);
    ::memcpy((void*)y_,(void*)y_ref_, sizeof(double)*npoints_);
    ::memcpy((void*)z_,(void*)z_ref_, sizeof(double)*npoints_);
    ::memcpy((void*)w_,(void*)w_ref_, sizeof(double)*npoints_);

    blocks_.clear();
    for (int Q = 0; Q < npoints_; Q += max_points_) {
        int n = (Q + max_points_ >= npoints_ ? npoints_ - Q : max_points_);
        blocks_.push_back(boost::shared_ptr<BlockOPoints>(new BlockOPoints(n,&x_[Q],&y_[Q],&z_[Q],&w_[Q], extents_))); 
    }
}
OctreeGridBlocker::OctreeGridBlocker(const int npoints_ref, double const* x_ref, double const* y_ref, double const* z_ref,
        double const* w_ref, const int max_points, const int min_points, boost::shared_ptr<BasisExtents> extents) :
    GridBlocker(npoints_ref,x_ref,y_ref,z_ref,w_ref,max_points,min_points,extents)
{
}
OctreeGridBlocker::~OctreeGridBlocker()
{
}
void OctreeGridBlocker::block()
{
    npoints_ = npoints_ref_;
    max_points_ = tol_max_points_;
    max_functions_ = extents_->basis()->nbf();

    double const* x = x_ref_;
    double const* y = y_ref_;
    double const* z = z_ref_;
    double const* w = w_ref_;

    std::vector<std::vector<int> > active_tree(1);
    std::vector<std::vector<int> > completed_tree(0);
    std::vector<std::vector<int> > new_leaves(0);

    for (int Q = 0; Q < npoints_; Q++) {
        active_tree[0].push_back(Q);
    }

    // K-PR Tree blocking    
    FILE* fh_ktree;
    if (bench_) {
        fh_ktree = fopen("ktree.dat","w"); 
        //fprintf(fh_ktree,"#  %4s %5s %15s %15s %15s\n", "Dept","ID","X","Y","Z");
    }
    int tree_level = 0;
    while (true) {
            
        // X
        new_leaves.clear();
        double const* X = x;
        for (int A = 0; A < active_tree.size(); A++) {
            
            // Block to subdivide
            std::vector<int> block = active_tree[A];
            
            // Determine xcenter of mass
            double xc = 0.0; 
            for (int Q = 0; Q < block.size(); Q++) {
                xc += X[block[Q]];
            } 
            xc /= block.size();    

            double XC[3]; ::memset((void*) XC, '\0', 3*sizeof(double));
            for (int Q = 0; Q < block.size(); Q++) {
                XC[0] += x[block[Q]];
                XC[1] += y[block[Q]];
                XC[2] += z[block[Q]];
            }
            XC[0] /= block.size();
            XC[1] /= block.size();
            XC[2] /= block.size();
            if (bench_) fprintf(fh_ktree,"   %4d %5d %15.6E %15.6E %15.6E\n", tree_level,A, XC[0],XC[1],XC[2]);

            std::vector<int> left;
            std::vector<int> right;
            for (int Q = 0; Q < block.size(); Q++) {
                if (X[block[Q]] < xc) {
                    left.push_back(block[Q]);
                } else {
                    right.push_back(block[Q]);
                }
            }

            if (left.size() > tol_max_points_) {
                new_leaves.push_back(left);        
            } else {
                completed_tree.push_back(left);
            }

            if (right.size() > tol_max_points_) {
                new_leaves.push_back(right);        
            } else {
                completed_tree.push_back(right);
            }
        }
        active_tree = new_leaves;
        tree_level++;
        if (!active_tree.size()) break;
            
        // Y
        new_leaves.clear();
        X = y;
        for (int A = 0; A < active_tree.size(); A++) {
            
            // Block to subdivide
            std::vector<int> block = active_tree[A];
            
            // Determine xcenter of mass
            double xc = 0.0; 
            for (int Q = 0; Q < block.size(); Q++) {
                xc += X[block[Q]];
            } 
            xc /= block.size();    

            double XC[3]; ::memset((void*) XC, '\0', 3*sizeof(double));
            for (int Q = 0; Q < block.size(); Q++) {
                XC[0] += x[block[Q]];
                XC[1] += y[block[Q]];
                XC[2] += z[block[Q]];
            }
            XC[0] /= block.size();
            XC[1] /= block.size();
            XC[2] /= block.size();
            if (bench_) fprintf(fh_ktree,"   %4d %5d %15.6E %15.6E %15.6E\n", tree_level,A, XC[0],XC[1],XC[2]);

            std::vector<int> left;
            std::vector<int> right;
            for (int Q = 0; Q < block.size(); Q++) {
                if (X[block[Q]] < xc) {
                    left.push_back(block[Q]);
                } else {
                    right.push_back(block[Q]);
                }
            }

            if (left.size() > tol_max_points_) {
                new_leaves.push_back(left);        
            } else {
                completed_tree.push_back(left);
            }

            if (right.size() > tol_max_points_) {
                new_leaves.push_back(right);        
            } else {
                completed_tree.push_back(right);
            }
        }
        active_tree = new_leaves;
        tree_level++;
        if (!active_tree.size()) break;
            
        // Z
        new_leaves.clear();
        X = z;
        for (int A = 0; A < active_tree.size(); A++) {
            
            // Block to subdivide
            std::vector<int> block = active_tree[A];
            
            // Determine xcenter of mass
            double xc = 0.0; 
            for (int Q = 0; Q < block.size(); Q++) {
                xc += X[block[Q]];
            } 
            xc /= block.size();    

            double XC[3]; ::memset((void*) XC, '\0', 3*sizeof(double));
            for (int Q = 0; Q < block.size(); Q++) {
                XC[0] += x[block[Q]];
                XC[1] += y[block[Q]];
                XC[2] += z[block[Q]];
            }
            XC[0] /= block.size();
            XC[1] /= block.size();
            XC[2] /= block.size();
            if (bench_) fprintf(fh_ktree,"   %4d %5d %15.6E %15.6E %15.6E\n", tree_level,A, XC[0],XC[1],XC[2]);

            std::vector<int> left;
            std::vector<int> right;
            for (int Q = 0; Q < block.size(); Q++) {
                if (X[block[Q]] < xc) {
                    left.push_back(block[Q]);
                } else {
                    right.push_back(block[Q]);
                }
            }

            if (left.size() > tol_max_points_) {
                new_leaves.push_back(left);        
            } else {
                completed_tree.push_back(left);
            }

            if (right.size() > tol_max_points_) {
                new_leaves.push_back(right);        
            } else {
                completed_tree.push_back(right);
            }
        }
        active_tree = new_leaves;
        tree_level++;
        if (!active_tree.size()) break;

    }
    if (bench_) fclose(fh_ktree);
    
    
    // Move stuff over
    x_ = new double[npoints_]; 
    y_ = new double[npoints_]; 
    z_ = new double[npoints_]; 
    w_ = new double[npoints_]; 

    int index = 0;
    int unique_block = 0;
    FILE* fh_blocks;
    if (bench_) {
        fh_blocks = fopen("finished_blocks.dat","w");
        //fprintf(fh_blocks, "#  %4s %15s %15s %15s %15s\n", "ID", "X", "Y", "Z", "W"); 
    }
    for (int A = 0; A < completed_tree.size(); A++) {
        std::vector<int> block = completed_tree[A];
        for (int Q = 0; Q < block.size(); Q++) {
            int delta = block[Q];
            x_[index] = x[delta];
            y_[index] = y[delta];
            z_[index] = z[delta];
            w_[index] = w[delta];
            if (bench_) fprintf(fh_blocks, "   %4d %15.6E %15.6E %15.6E %15.6E\n", unique_block, x_[index], y_[index], z_[index], w_[index]); 
            index++;
        } 
        if (block.size()) unique_block++;
    }
    if (bench_) fclose(fh_blocks);

    index = 0;
    max_points_ = 0;
    for (int A = 0; A < completed_tree.size(); A++) {
        std::vector<int> block = completed_tree[A];
        if (!block.size()) continue;
        blocks_.push_back(boost::shared_ptr<BlockOPoints>(new BlockOPoints(block.size(),&x_[index],&y_[index],&z_[index],&w_[index],extents_)));
        if (max_points_ < block.size()) {
            max_points_ = block.size();
        }
        index += block.size();
    }

    max_functions_ = 0;
    for (int A = 0; A < blocks_.size(); A++) {
        if (max_functions_ < blocks_[A]->functions_local_to_global().size())
            max_functions_ = blocks_[A]->functions_local_to_global().size();
    } 

    if (bench_) {
        FILE* fh_extents = fopen("extents.dat","w");
        //fprintf(fh_extents,"    %4s %15s %15s %15s %15s\n","ID","X","Y","Z","R");
        boost::shared_ptr<BasisSet> basis = extents_->basis();
        boost::shared_ptr<Vector> Rc = extents_->shell_extents();         
        for (int Q = 0; Q < basis->nshell(); Q++) {
            Vector3 v = basis->shell(Q)->center();
            fprintf(fh_extents,"    %4d %15.6E %15.6E %15.6E %15.6E\n", Q,v[0],v[1],v[2],Rc->get(0,Q));
        }
        fclose(fh_extents);
    }

    if (bench_ > 2) {
        double delta2 = extents_->delta();
        for (int i = 2; i < 20; i++) { 
            std::stringstream ss;
            ss << "extents" << i << ".dat";
            FILE* fh_extents = fopen(ss.str().c_str(),"w");
            //fprintf(fh_extents,"    %4s %15s %15s %15s %15s\n","ID","X","Y","Z","R");
            extents_->set_delta(pow(10.0,-i));
            boost::shared_ptr<BasisSet> basis = extents_->basis();
            boost::shared_ptr<Vector> Rc = extents_->shell_extents();         
            for (int Q = 0; Q < basis->nshell(); Q++) {
                Vector3 v = basis->shell(Q)->center();
                fprintf(fh_extents,"    %4d %15.6E %15.6E %15.6E %15.6E\n", Q,v[0],v[1],v[2],Rc->get(0,Q));
            }
            fclose(fh_extents);
        }
        extents_->set_delta(delta2);
    }
}

}
