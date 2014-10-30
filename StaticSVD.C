/******************************************************************************
 *
 * This file is part of the CAROM distribution.  For full copyright
 * information, see COPYRIGHT.
 *
 * Copyright:   (c) 2013-2014 Lawrence Livermore National Security, LLC
 * Description: A class implementing the static SVD algorithm.
 *
 *****************************************************************************/

#include "StaticSVD.h"

#include "mpi.h"

#include <stdio.h>
#include <string.h>

extern "C" {
void dgesdd_(char*, int*, int*, double*, int*,
             double*, double*, int*, double*, int*,
             double*, int*, int*, int*);
}

namespace CAROM {

const int StaticSVD::COMMUNICATE_A = 999;

StaticSVD::StaticSVD(
   int dim,
   int increments_per_time_interval) :
   d_dim(dim),
   d_num_increments(0),
   d_increments_per_time_interval(increments_per_time_interval),
   d_state(0),
   d_U(0),
   d_S(0),
   d_V(0),
   d_basis(0),
   d_num_time_intervals(0),
   d_time_interval_start_times(0),
   d_this_interval_basis_current(false)
{
   // Get the rank of this process, and get the number of processors.
   int mpi_init;
   MPI_Initialized(&mpi_init);
   if (mpi_init) {
      MPI_Comm_rank(MPI_COMM_WORLD, &d_rank);
      MPI_Comm_size(MPI_COMM_WORLD, &d_size);
   }
   else {
      d_rank = 0;
      d_size = 1;
   }
}

StaticSVD::~StaticSVD()
{
   // Delete data members.
   if (d_U) {
      delete d_U;
   }
   if (d_S) {
      delete d_S;
   }
   if (d_V) {
      delete d_V;
   }
   for (int i = 0; i < static_cast<int>(d_state.size()); ++i) {
      if (d_state[i]) {
         delete [] d_state[i];
      }
   }
   if (d_basis) {
      delete d_basis;
   }
}

void
StaticSVD::collectState(
   double* u_in,
   double time)
{
   if (isNewTimeInterval()) {

      // We have a new time interval.
      if (d_num_time_intervals > 0) {
         if (d_basis) {
            delete d_basis;
            d_basis = 0;
         }
         for (int i = 0; i < static_cast<int>(d_state.size()); ++i) {
            if (d_state[i]) {
               delete [] d_state[i];
            }
         }
         d_state.resize(0);
         delete d_U;
         d_U = 0;
         delete d_S;
         d_S = 0;
         delete d_V;
         d_V = 0;
      }
      d_num_increments = 0;
      ++d_num_time_intervals;
      d_time_interval_start_times.resize(d_num_time_intervals);
      d_time_interval_start_times[d_num_time_intervals-1] = time;
      d_basis = 0;
   }
   double* state = new double [d_dim];
   memcpy(state, u_in, d_dim*sizeof(double));
   d_state.push_back(state);
   ++d_num_increments;
   d_this_interval_basis_current = false;
}

const Matrix*
StaticSVD::getBasis()
{
   // If this basis is for the last time interval then it may not be up to date
   // so recompute it.
   if (!d_this_interval_basis_current) {
      if (d_basis != 0) {
         delete d_basis;
      }
      computeSVD();
   }
   else {
      CAROM_ASSERT(d_basis != 0);
   }
   CAROM_ASSERT(d_this_interval_basis_current);
   return d_basis;
}

void
StaticSVD::computeSVD()
{
   // Do this computation on process 0 and broadcast results to the other
   // processes.
   int num_cols = static_cast<int>(d_state.size());
   if (d_rank == 0) {
      // Construct storage for the global state of A.
      double* A = new double [d_dim*d_size*num_cols];

      // Put this processor's contribution to A into it in column major order.
      int idx = 0;
      for (int col = 0; col < num_cols; ++col) {
         double* col_vals = d_state[col];
         for (int row = 0; row < d_dim; ++row) {
            A[idx+row] = col_vals[row];
         }
         idx += d_dim*d_size;
      }

      // Get the contributions to the global state from the other processes.
      if (d_size > 1) {
         double* Aproc = new double [d_dim*num_cols];
         for (int proc = 1; proc < d_size; ++proc) {
            MPI_Status status;
            MPI_Recv(Aproc, d_dim*num_cols, MPI_DOUBLE, proc,
                     COMMUNICATE_A, MPI_COMM_WORLD, &status);
            int Aproc_idx = 0;
            for (int col = 0; col < num_cols; ++col) {
               int Aidx = col*d_dim*d_size + d_dim*proc;
               for (int row = 0; row < d_dim; ++row) {
                  A[Aidx++] = Aproc[Aproc_idx++];
               }
            }
         }
         delete [] Aproc;
      }

      // Perform the svd.
      svd(A);

      // Broadcast the results.
      if (d_size > 1) {
         MPI_Bcast(&d_U->item(0, 0), d_dim*d_size*num_cols,
                   MPI_DOUBLE, 0, MPI_COMM_WORLD);
         MPI_Bcast(&d_S->item(0, 0), num_cols*num_cols,
                   MPI_DOUBLE, 0, MPI_COMM_WORLD);
         MPI_Bcast(&d_V->item(0, 0), num_cols*num_cols,
                   MPI_DOUBLE, 0, MPI_COMM_WORLD);
      }

      // Clean up.
      delete [] A;
   }
   else {
      // Put this processor's contribution to the global state of A into myA
      // in column major order.
      double* myA = new double [d_dim*num_cols];
      int idx = 0;
      for (int col = 0; col < num_cols; ++col) {
         double* col_vals = d_state[col];
         for (int row = 0; row < d_dim; ++row) {
            myA[idx++] = col_vals[row];
         }
      }

      // Send the contribution to the global state of A to process 0.
      MPI_Request request;
      MPI_Isend(myA, d_dim*num_cols, MPI_DOUBLE, 0,
                COMMUNICATE_A, MPI_COMM_WORLD, &request);

      // Allocate d_U, d_S, and d_V.
      d_U = new Matrix(d_dim*d_size, num_cols, false);
      d_S = new Matrix(num_cols, num_cols, false);
      d_V = new Matrix(num_cols, num_cols, false);

      // Get the results from process 0.
      MPI_Bcast(&d_U->item(0, 0), d_dim*d_size*num_cols,
                MPI_DOUBLE, 0, MPI_COMM_WORLD);
      MPI_Bcast(&d_S->item(0, 0), num_cols*num_cols,
                MPI_DOUBLE, 0, MPI_COMM_WORLD);
      MPI_Bcast(&d_V->item(0, 0), num_cols*num_cols,
                MPI_DOUBLE, 0, MPI_COMM_WORLD);

      // Clean up.
      delete [] myA;
   }
   d_basis = new Matrix(*d_U);
   d_this_interval_basis_current = true;
#ifdef DEBUG_ROMS
   if (d_rank == 0) {
      for (int row = 0; row < d_dim*d_size; ++row) {
         for (int col = 0; col < num_cols; ++col) {
            printf("%.16e ", d_U->item(row, col));
         }
         printf("\n");
      }
      printf("\n");
      for (int row = 0; row < num_cols; ++row) {
         for (int col = 0; col < num_cols; ++col) {
            printf("%.16e ", d_S->item(row, col));
         }
         printf("\n");
      }
   }
#endif
}

void
StaticSVD::svd(
   double* A)
{
   int num_states = static_cast<int>(d_state.size());

   // Construct d_U.
   d_U = new Matrix(d_dim*d_size, num_states, false);

   // Construct d_S.
   d_S = new Matrix(num_states, num_states, false);
   for (int row = 0; row < num_states; ++row) {
      for (int col = 0; col < num_states; ++col) {
         d_S->item(row, col) = 0.0;
      }
   }

   // Construct d_V.
   d_V = new Matrix(num_states, num_states, false);

   // Use lapack's dgesdd_ Fortran function to perform the svd.  As this is
   // Fortran A and all the computed matrices are in column major order.
   char jobz = 'A';
   int m = d_dim*d_size;
   int n = num_states;
   int lda = m;
   double* sigma = new double [num_states];
   double* U = new double[m*m];
   int ldu = m;
   int ldv = num_states;
   int lwork = n*(4*n + 6)+m;
   double* work = new double [lwork];
   int* iwork = new int [8*n];
   int info;
   dgesdd_(&jobz, &m, &n, A, &lda,
           sigma, U, &ldu, &d_V->item(0, 0), &ldv,
           work, &lwork, iwork, &info);
   CAROM_ASSERT(info == 0);
   delete [] work;
   delete [] iwork;

   // Place sigma into d_S.
   for (int i = 0; i < num_states; ++i) {
      d_S->item(i, i) = sigma[i];
   }
   delete [] sigma;

   // Take the first n rows of U and place into d_U.  U is in column major
   // order so convert is to row major order as this is done.
   int uidx = 0;
   for (int row = 0; row < n; ++row) {
      for (int col = 0; col < m; ++col) {
         d_U->item(col, row) = U[uidx++];
      }
   }
   delete [] U;

   // d_V is in column major order.  Convert it to row major order.
   for (int row = 0; row < num_states; ++row) {
      for (int col = row+1; col < num_states; ++col) {
         double tmp = d_V->item(row, col);
         d_V->item(row, col) = d_V->item(col, row);
         d_V->item(col, row) = tmp;
      }
   }
}

}