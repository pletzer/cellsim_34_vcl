/*
 * cMKLSolver.cpp
 *
 *  Created on: May 10, 2016
 *      Author: Chris Scott
 */

#ifdef MKL_SOLVER

#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <Eigen/Sparse>
#include <mkl.h>

#include "cMKLSolver.h"
#include "cCellMesh.h"

cMKLSolver::cMKLSolver(MatrixXXC &Amat) {
    // Amat should be column major (the default for Eigen) as that is what MKL expects
    if (Amat.IsRowMajor) {
        fatal_error("Supplied dense matrix must be column major!");
    }
    
	std::cout << "<SOLVER> initialising the MKL solver..." << std::endl;
    std::cout << "<SOLVER> storing A matrix in sparse format..." << std::endl;
    
    // size of A
    MKL_INT nrows = Amat.rows();
    MKL_INT ncols = Amat.cols();
    size = nrows;
    
    // calculate the number of non zeros (eigen matrix is column major)
    MKL_INT nnz = 0;
    for (MKL_INT j = 0; j < ncols; j++) {
        for (MKL_INT i = 0; i < nrows; i++) {
            if (Amat(i, j) != 0) nnz++;
        }
    }
    
    // define the job for converting to CSR
    MKL_INT job[8];
    job[0] = 0;  // convert from dense to sparse
    job[1] = 0;  // zero based indexing for A matrix
    job[2] = 1;  // one based indexing for CSR matrix (apparently required for gmres!)
    job[3] = 2;  // adns is whole matrix
    job[4] = nnz;  // maximum number of non zeros allowed
    job[5] = 1;  // fill all three CSR arrays
    
    // other parameters
    MKL_INT lda = nrows;
    MKL_INT info;
    
    // pointer to data
    double *adns = Amat.data();
    
    // allocate CSR arrays for A
    Acsr = new double[nnz];
    Aj = new MKL_INT[nnz];
    Ai = new MKL_INT[nrows + 1];
    
    // convert to MKL CSR format
    mkl_ddnscsr(job, &nrows, &ncols, adns, &lda, Acsr, Aj, Ai, &info);
    if (info != 0) {
        fatal_error("conversion to CSR matrix failed!");
    }
    
    std::cout << "<SOLVER> computing preconditioner..." << std::endl;
    
    // set up the preconditioner
    MKL_INT ipar[128];
    ipar[1] = 6;  // output of error messages to the screen,
    ipar[5] = 1;  // allow output of errors,
    ipar[6] = 1;  // output warn messages if any and continue
    ipar[30] = 0;  // abort DCSRILUT calculations if routine meets zero diagonal element.
    double dpar[128];
    dpar[30] = 1.e-5;
    // ilut preconditioner parameters
    double tol = 1.e-5;
    MKL_INT maxfil = 60;
    
    // allocate storage for preconditioner
    MKL_INT ilut_size = (2 * maxfil + 1) * size - maxfil * (maxfil + 1) + 1;
    bilut = new double[ilut_size];
    ibilut = new MKL_INT[size + 1];
    jbilut = new MKL_INT[ilut_size];
    
    // compute preconditioner
    MKL_INT ierr;
    dcsrilut(&size, Acsr, Ai, Aj, bilut, ibilut, jbilut, &tol, &maxfil, ipar, dpar, &ierr);
    if (ierr != 0) {
        std::ostringstream msgstream;
        msgstream << "<SOLVER> Error: computation of preconditioner failed (error code " << ierr << ")!";
        fatal_error(msgstream.str());
    }
    
    // gmres parameters
    gmres_restarts = 20;  // how often to restart gmres
    gmres_relative_tol = 0.0;
    gmres_absolute_tol = 1.e-8;  // for comparison with VCL
    
    // allocate workspace for gmres
    MKL_INT tmpsize = ((2 * gmres_restarts + 1) * size + gmres_restarts * (gmres_restarts + 9) / 2 + 1);
    gmres_tmp = new double[tmpsize];
    gmres_trvec = new double[size];
}

cMKLSolver::~cMKLSolver() {
    delete [] Acsr;
    delete [] Aj;
    delete [] Ai;
    delete [] bilut;
    delete [] ibilut;
    delete [] jbilut;
    delete [] gmres_tmp;
    delete [] gmres_trvec;
}

void cMKLSolver::step(MatrixX1C &solvec, MatrixX1C &rhsvec) {
    // variables required by gmres
    MKL_INT RCI_request;
    MKL_INT itercount;
    MKL_INT ivar = size;
    MKL_INT ipar[128];
    double dpar[128];
    
    // pointers to vectors
    double *solution = solvec.data();
    double *rhs = rhsvec.data();
    
    // zero initial guess
    for (int i = 0; i < size; i++) {
        solution[i] = 0.0;
    }
    
    // initialise gmres
    dfgmres_init(&ivar, solution, rhs, &RCI_request, ipar, dpar, gmres_tmp);
    if (RCI_request != 0) {
        fatal_error("initialising gmres failed!");
    }
    
    // set desired parameters
    ipar[7] = 1;  // perform maximum iterations test
    ipar[8] = 1;  // perform residual stopping test
    ipar[9] = 0;  // do not perform the user defined stopping test
    ipar[10] = 1;  // run preconditioned gmres
    ipar[14] = gmres_restarts;  // how often to restart gmres
    dpar[0] = gmres_relative_tol;  // relative tolerance
    dpar[1] = gmres_absolute_tol;  // absolute tolerance
    
    // check correctness of parameters
    dfgmres_check(&ivar, solution, rhs, &RCI_request, ipar, dpar, gmres_tmp);
    if (RCI_request != 0) {
        fatal_error("param check failed!");
    }
    
    // start gmres reverse communication
    dfgmres(&ivar, solution, rhs, &RCI_request, ipar, dpar, gmres_tmp);
    bool complete = false;
    while (not complete) {
        // success
        if (RCI_request == 0) {
            complete = true;
        }
        // compute matrix vector multiplication
        else if (RCI_request == 1) {
            // compute gmres_tmp[ipar[22]-1] = A*gmres_tmp[ipar[21]-1]
            // note: ipar[21] and ipar[22] contain fortran style addresses so we must subtract 1
            char cvar = 'N';
            mkl_dcsrgemv(&cvar, &ivar, Acsr, Ai, Aj, &gmres_tmp[ipar[21] - 1], &gmres_tmp[ipar[22] - 1]);
        }
        // apply the preconditioner
        else if (RCI_request == 3) {
            char cvar = 'N';
            char cvar1 = 'L';
            char cvar2 = 'U';
            mkl_dcsrtrsv(&cvar1, &cvar, &cvar2, &ivar, bilut, ibilut, jbilut, &gmres_tmp[ipar[21] - 1], gmres_trvec);
            cvar1='U';
    		cvar='N';
    		cvar2='N';
    		mkl_dcsrtrsv(&cvar1, &cvar, &cvar2, &ivar, bilut, ibilut, jbilut, gmres_trvec, &gmres_tmp[ipar[22] - 1]);
        }
        // check norm of generated vector
        else if (RCI_request == 4) {
            if (dpar[6] < 1.e-12) {
                complete = true;
            }
        }
        // failed
        else {
            std::ostringstream msgstream;
            msgstream << "fgmres failed: RCI_request " << RCI_request << "!";
            fatal_error(msgstream.str());
        }
        
        // next gmres call
        if (not complete) {
            dfgmres(&ivar, solution, rhs, &RCI_request, ipar, dpar, gmres_tmp);
        }
    }
    
    // get the result and print iteration count
    ipar[12] = 0;
    dfgmres_get(&ivar, solution, rhs, &RCI_request, ipar, dpar, gmres_tmp, &itercount);
    std::cout << itercount << " " << dpar[4] << std::endl;
}

void cMKLSolver::fatal_error(std::string msg) {
    std::cerr << "<SOLVER> ERROR: " << msg << std::endl;
	exit(1);
}

#endif
