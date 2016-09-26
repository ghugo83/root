// @(#)root/tmva/tmva/dnn:$Id$
// Author: Simon Pfreundschuh 20/07/16

/*************************************************************************
 * Copyright (C) 2016, Simon Pfreundschuh                                *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

 /////////////////////////////////////////////////////////////////////
 // Implementation of the loss functions for the multi-threaded CPU //
 // implementation using Roots ThreadPool and BLAS.                 //
 /////////////////////////////////////////////////////////////////////

#include "TMVA/DNN/Architectures/Reference.h"

namespace TMVA
{
namespace DNN
{

//______________________________________________________________________________
template<typename AFloat>
AFloat TCpu<AFloat>::MeanSquaredError(const TCpuMatrix<AFloat> &Y,
                                      const TCpuMatrix<AFloat> &output)
{
   const AFloat  *dataY      = Y.GetRawDataPointer();
   const AFloat  *dataOutput = output.GetRawDataPointer();
   std::vector<AFloat> temp(Y.GetNElements());
   AFloat norm = 1.0 / ((AFloat) Y.GetNrows() * Y.GetNcols());

   auto f = [&dataY, &dataOutput, &temp](UInt_t workerID)
   {
      AFloat dy = dataY[workerID] - dataOutput[workerID];
      temp[workerID] = dy * dy;
      return 0;
   };

   auto reduction = [](AFloat sum1, AFloat sum2)
   {
      return sum1 + sum2;
   };

   Y.GetThreadPool().Map(f, ROOT::TSeqI(Y.GetNElements()));
   return norm * Y.GetThreadPool().Reduce(temp, reduction);
}

//______________________________________________________________________________
template<typename AFloat>
void TCpu<AFloat>::MeanSquaredErrorGradients(
    TCpuMatrix<AFloat> & dY,
    const TCpuMatrix<AFloat> & Y,
    const TCpuMatrix<AFloat> & output)
{

         AFloat  *dataDY     = dY.GetRawDataPointer();
   const AFloat  *dataY      = Y.GetRawDataPointer();
   const AFloat  *dataOutput = output.GetRawDataPointer();
   AFloat norm = 1.0 / ((AFloat) Y.GetNrows() * Y.GetNcols());

   auto f = [&dataDY, &dataY, &dataOutput, norm](UInt_t workerID)
   {
      dataDY[workerID] = - 2.0 * norm * (dataY[workerID] - dataOutput[workerID]);
      return 0;
   };

   Y.GetThreadPool().Map(f, ROOT::TSeqI(Y.GetNElements()));
}

//______________________________________________________________________________
template<typename AFloat>
AFloat TCpu<AFloat>::CrossEntropy(const TCpuMatrix<AFloat> &Y,
                                  const TCpuMatrix<AFloat> &output)
{
   const AFloat  *dataY      = Y.GetRawDataPointer();
   const AFloat  *dataOutput = output.GetRawDataPointer();
   std::vector<AFloat> temp(Y.GetNElements());
   AFloat norm = 1.0 / ((AFloat) Y.GetNrows() * Y.GetNcols());

   auto f = [&dataY, &dataOutput, &temp](UInt_t workerID)
   {
      AFloat y   = dataY[workerID];
      AFloat sig = 1.0 / (1.0 + exp(- dataOutput[workerID]));
      temp[workerID] = - (y * log(sig) + (1.0 - y) * log(1.0 - sig));
      return 0;
   };

   auto reduction = [](AFloat sum1, AFloat sum2)
   {
      return sum1 + sum2;
   };

   Y.GetThreadPool().Map(f, ROOT::TSeqI(Y.GetNElements()));
   return norm * Y.GetThreadPool().Reduce(temp, reduction);
}

//______________________________________________________________________________
template<typename AFloat>
void TCpu<AFloat>::CrossEntropyGradients(
    TCpuMatrix<AFloat> & dY,
    const TCpuMatrix<AFloat> & Y,
    const TCpuMatrix<AFloat> & output)
{
         AFloat  *dataDY     = dY.GetRawDataPointer();
   const AFloat  *dataY      = Y.GetRawDataPointer();
   const AFloat  *dataOutput = output.GetRawDataPointer();
   AFloat norm = 1.0 / ((AFloat) Y.GetNrows() * Y.GetNcols());

   auto f = [&dataDY, &dataY, &dataOutput, norm](UInt_t workerID)
   {
      AFloat y   = dataY[workerID];
      AFloat sig = 1.0 / (1.0 + exp(- dataOutput[workerID]));
      dataDY[workerID] = norm * (sig - y);
      return 0;
   };

   Y.GetThreadPool().Map(f, ROOT::TSeqI(Y.GetNElements()));
}

} // namespace DNN
} // namespace TMVA