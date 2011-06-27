/*! \file Peridigm_PdQuickGridDiscretization.cpp */

//@HEADER
// ************************************************************************
//
//                             Peridigm
//                 Copyright (2011) Sandia Corporation
//
// Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
// the U.S. Government retains certain rights in this software.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions?
// David J. Littlewood   djlittl@sandia.gov
// John A. Mitchell      jamitch@sandia.gov
// Michael L. Parks      mlparks@sandia.gov
// Stewart A. Silling    sasilli@sandia.gov
//
// ************************************************************************
//@HEADER

#include "Peridigm_PdQuickGridDiscretization.hpp"
#include "mesh_input/quick_grid/QuickGrid.h"
#include "pdneigh/PdZoltan.h"
#include "utilities/Vector.h"
#include "utilities/Array.h"
#include <vector>
#include <list>
#include <sstream>
#include <tr1/memory>

using namespace std;
using tr1::shared_ptr;

/*
 * Private Prototypes
 */
namespace PeridigmNS {
	const Epetra_BlockMap getOverlap(int ndf, int numShared, int*shared, int numOwned,const  int* owned, const Epetra_Comm& comm);
	UTILITIES::Array<int> getSharedGlobalIds(const QUICKGRID::Data& gridData);
	shared_ptr<int> getLocalOwnedIds(const QUICKGRID::Data& gridData, const Epetra_BlockMap& overlapMap);
	shared_ptr<int> getLocalNeighborList(const QUICKGRID::Data& gridData, const Epetra_BlockMap& overlapMap);
}



PeridigmNS::PdQuickGridDiscretization::PdQuickGridDiscretization(const Teuchos::RCP<const Epetra_Comm>& epetra_comm,
                                                                 const Teuchos::RCP<Teuchos::ParameterList>& params) :
  comm(epetra_comm),
  numBonds(0),
  myPID(comm->MyPID()),
  numPID(comm->NumProc())
{
  TEST_FOR_EXCEPT_MSG(params->get<string>("Type") != "PdQuickGrid", "Invalid Type in PdQuickGridDiscretization");
  QUICKGRID::Data decomp = getDiscretization(params);

  createMaps(decomp);
  createNeighborhoodData(decomp);

  // Create the bondMap, a local map used for constitutive data stored on bonds.
  // Due to Epetra_BlockMap restrictions, there can not be any entries with length zero.
  // This means that points with no neighbors can not appear in the bondMap.
  int numMyElementsUpperBound = oneDimensionalMap->NumMyElements();
  int numGlobalElements = -1; 
  int numMyElements = 0;
  int* oneDimensionalMapGlobalElements = oneDimensionalMap->MyGlobalElements();
  int* myGlobalElements = new int[numMyElementsUpperBound];
  int* elementSizeList = new int[numMyElementsUpperBound];
  int* neighborhood = decomp.neighborhood.get();
  int neighborhoodIndex = 0;
  int numPointsWithZeroNeighbors = 0;
  for(int i=0 ; i<decomp.numPoints ; ++i){
    int numNeighbors = neighborhood[neighborhoodIndex];
    if(numNeighbors > 0){
      numMyElements++;
      myGlobalElements[i-numPointsWithZeroNeighbors] = oneDimensionalMapGlobalElements[i];
      elementSizeList[i-numPointsWithZeroNeighbors] = numNeighbors;
    }
    else{
      numPointsWithZeroNeighbors++;
    }
    numBonds += numNeighbors;
    neighborhoodIndex += 1 + numNeighbors;
  }
  int indexBase = 0;
  bondMap = Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, myGlobalElements, elementSizeList, indexBase, *comm));
  delete[] myGlobalElements;
  delete[] elementSizeList;

  // 3D only
  TEST_FOR_EXCEPT_MSG(decomp.dimension != 3, "Invalid dimension in decomposition (only 3D is supported)");

  // fill the x vector with the current positions (owned positions only)
  initialX = Teuchos::rcp(new Epetra_Vector(Copy,*threeDimensionalMap,decomp.myX.get()) );

  // fill cell volumes
  cellVolume = Teuchos::rcp(new Epetra_Vector(Copy,*oneDimensionalMap,decomp.cellVolume.get()) );
}


PeridigmNS::PdQuickGridDiscretization::PdQuickGridDiscretization(const Teuchos::RCP<const Epetra_Comm>& epetra_comm,
                                                                 const Teuchos::RCP<const QUICKGRID::Data>& decomp) :
  comm(epetra_comm),
  numBonds(0),
  myPID(comm->MyPID()),
  numPID(comm->NumProc())
{
  createMaps(*decomp);
  createNeighborhoodData(*decomp);

  // Create the bondMap, a local map used for constitutive data stored on bonds.
  // Due to Epetra_BlockMap restrictions, there can not be any entries with length zero.
  // This means that points with no neighbors can not appear in the bondMap.
  int numMyElementsUpperBound = oneDimensionalMap->NumMyElements();
  int numGlobalElements = -1; 
  int numMyElements = 0;
  int* oneDimensionalMapGlobalElements = oneDimensionalMap->MyGlobalElements();
  int* myGlobalElements = new int[numMyElementsUpperBound];
  int* elementSizeList = new int[numMyElementsUpperBound];
  int* neighborhood = decomp->neighborhood.get();
  int neighborhoodIndex = 0;
  int numPointsWithZeroNeighbors = 0;
  for(int i=0 ; i<decomp->numPoints ; ++i){
    int numNeighbors = neighborhood[neighborhoodIndex];
    if(numNeighbors > 0){
      numMyElements++;
      myGlobalElements[i-numPointsWithZeroNeighbors] = oneDimensionalMapGlobalElements[i];
      elementSizeList[i-numPointsWithZeroNeighbors] = numNeighbors;
    }
    else{
      numPointsWithZeroNeighbors++;
    }
    numBonds += numNeighbors;
    neighborhoodIndex += 1 + numNeighbors;
  }
  int indexBase = 0;
  bondMap = Teuchos::rcp(new Epetra_BlockMap(numGlobalElements, numMyElements, myGlobalElements, elementSizeList, indexBase, *comm));
  delete[] myGlobalElements;
  delete[] elementSizeList;

  // 3D only
  TEST_FOR_EXCEPT_MSG(decomp->dimension != 3, "Invalid dimension in decomposition (only 3D is supported)");

  // fill the x vector with the current positions (owned positions only)
  initialX = Teuchos::rcp(new Epetra_Vector(Copy, *threeDimensionalMap, decomp->myX.get()) );

  // fill cell volumes
  cellVolume = Teuchos::rcp(new Epetra_Vector(Copy, *oneDimensionalMap, decomp->cellVolume.get()) );
}

PeridigmNS::PdQuickGridDiscretization::~PdQuickGridDiscretization() {}

QUICKGRID::Data PeridigmNS::PdQuickGridDiscretization::getDiscretization(const Teuchos::RCP<Teuchos::ParameterList>& params) {

  // This is the type of norm used to create neighborhood lists
  QUICKGRID::NormFunctionPointer neighborhoodType = QUICKGRID::NoOpNorm;
  Teuchos::ParameterEntry* normTypeEntry=params->getEntryPtr("NeighborhoodType");
  if(NULL!=normTypeEntry){
    std::string normType = params->get<string>("NeighborhoodType");
    if(normType=="Spherical") neighborhoodType = QUICKGRID::SphericalNorm;
  }

  // Get the horizion
  horizon = params->get<double>("Horizon");

  // param list should have a "sublist" with different types that we switch on here
  QUICKGRID::Data decomp;
  if (params->isSublist("TensorProduct3DMeshGenerator")){
    Teuchos::RCP<Teuchos::ParameterList> pdQuickGridParamList = Teuchos::rcp(&(params->sublist("TensorProduct3DMeshGenerator")), false);
    double xStart = pdQuickGridParamList->get<double>("X Origin");
    double yStart = pdQuickGridParamList->get<double>("Y Origin");
    double zStart = pdQuickGridParamList->get<double>("Z Origin");
    double xLength = pdQuickGridParamList->get<double>("X Length");
    double yLength = pdQuickGridParamList->get<double>("Y Length");
    double zLength = pdQuickGridParamList->get<double>("Z Length");
    int nx = pdQuickGridParamList->get<int>("Number Points X");
    int ny = pdQuickGridParamList->get<int>("Number Points Y");
    int nz = pdQuickGridParamList->get<int>("Number Points Z");

    const QUICKGRID::Spec1D xSpec(nx,xStart,xLength);
    const QUICKGRID::Spec1D ySpec(ny,yStart,yLength);
    const QUICKGRID::Spec1D zSpec(nz,zStart,zLength);

    // Create abstract decomposition iterator
    QUICKGRID::TensorProduct3DMeshGenerator cellPerProcIter(numPID,horizon,xSpec,ySpec,zSpec,neighborhoodType);
    decomp =  QUICKGRID::getDiscretization(myPID, cellPerProcIter);
    // Load balance and write new decomposition
    #ifdef HAVE_MPI
      decomp = PDNEIGH::getLoadBalancedDiscretization(decomp);
    #endif
  } 
  else if (params->isSublist("TensorProductCylinderMeshGenerator")){
    Teuchos::RCP<Teuchos::ParameterList> pdQuickGridParamList = Teuchos::rcp(&(params->sublist("TensorProductCylinderMeshGenerator")), false);
    double innerRadius    = pdQuickGridParamList->get<double>("Inner Radius");
    double outerRadius    = pdQuickGridParamList->get<double>("Outer Radius");
    double cylinderLength = pdQuickGridParamList->get<double>("Cylinder Length");
    int numRings          = pdQuickGridParamList->get<int>("Number Points Radius");
    double xC             = pdQuickGridParamList->get<double>("Ring Center x");
    double yC             = pdQuickGridParamList->get<double>("Ring Center y");
    double zStart         = pdQuickGridParamList->get<double>("Z Origin");

    // Create 2d Ring
    UTILITIES::Vector3D center;
    center[0] = xC;
    center[1] = yC;
    center[2] = 0;

    // Note that zStart is used for the 1D spec along cylinder axis
    QUICKGRID::SpecRing2D ring2dSpec(center,innerRadius,outerRadius,numRings);

    // Create 1d Spec along cylinder axis
    // Compute number of cells along length of cylinder so that aspect ratio
    // is cells is approximately 1.
    // Cell sizes along axis are not exactly "cellSize" since last cell
    // would be a fraction of a cellSize -- so 1 is added to numCellsAlongAxis.
    // Actual cell sizes are slightly smaller than "cellSize" because of this.
    double cellSize = ring2dSpec.getRaySpec().getCellSize();
    int numCellsAxis = (int)(cylinderLength/cellSize)+1;
    QUICKGRID::Spec1D axisSpec(numCellsAxis,zStart,cylinderLength);

    // Create abstract decomposition iterator
    QUICKGRID::TensorProductCylinderMeshGenerator cellPerProcIter(numPID, horizon,ring2dSpec, axisSpec,neighborhoodType);
    decomp =  QUICKGRID::getDiscretization(myPID, cellPerProcIter);
    // Load balance and write new decomposition
    #ifdef HAVE_MPI
      decomp = PDNEIGH::getLoadBalancedDiscretization(decomp);
    #endif
  } 
  else { // ERROR
    TEST_FOR_EXCEPT_MSG(true, "Invalid Type in PdQuickGridDiscretization");
  }

  return decomp;
}

void
PeridigmNS::PdQuickGridDiscretization::createMaps(const QUICKGRID::Data& decomp)
{
  int dimension;

  // oneDimensionalMap
  // used for global IDs and scalar data
  dimension = 1;
  oneDimensionalMap = Teuchos::rcp(new Epetra_BlockMap(getOwnedMap(*comm, decomp, dimension)));

  // oneDimensionalOverlapMap
  // used for global IDs and scalar data, includes ghosts
  dimension = 1;
  oneDimensionalOverlapMap = Teuchos::rcp(new Epetra_BlockMap(getOverlapMap(*comm, decomp, dimension)));

  // threeDimensionalMap
  // used for R3 vector data, e.g., u, v, etc.
  dimension = 3;
  threeDimensionalMap = Teuchos::rcp(new Epetra_BlockMap(getOwnedMap(*comm, decomp, dimension)));

  // threeDimensionalOverlapMap
  // used for R3 vector data, e.g., u, v, etc.,  includes ghosts
  dimension = 3;
  threeDimensionalOverlapMap = Teuchos::rcp(new Epetra_BlockMap(getOverlapMap(*comm, decomp, dimension)));

}

void
PeridigmNS::PdQuickGridDiscretization::createNeighborhoodData(const QUICKGRID::Data& decomp)
{
   neighborhoodData = Teuchos::rcp(new PeridigmNS::NeighborhoodData);
   neighborhoodData->SetNumOwned(decomp.numPoints);
   memcpy(neighborhoodData->OwnedIDs(), getLocalOwnedIds(decomp, *oneDimensionalOverlapMap).get(),
 		  decomp.numPoints*sizeof(int));
   memcpy(neighborhoodData->NeighborhoodPtr(), 
 		  decomp.neighborhoodPtr.get(),
 		  decomp.numPoints*sizeof(int));
   neighborhoodData->SetNeighborhoodListSize(decomp.sizeNeighborhoodList);
   memcpy(neighborhoodData->NeighborhoodList(),
		  getLocalNeighborList(decomp, *oneDimensionalOverlapMap).get(),
 		  decomp.sizeNeighborhoodList*sizeof(int));
}

Teuchos::RCP<const Epetra_BlockMap>
PeridigmNS::PdQuickGridDiscretization::getMap(int d) const
{
  switch (d) {
    case 1:
      return oneDimensionalMap;
      break;
    case 3:
      return threeDimensionalMap;
      break;
    default:
      TEST_FOR_EXCEPTION(true, Teuchos::Exceptions::InvalidParameter, 
                         std::endl << "PdQuickGridDiscretization::getMap(int d) only supports dimensions d=1 or d=3. Supplied dimension d=" << d << std::endl); 
    }
}

Teuchos::RCP<const Epetra_BlockMap>
PeridigmNS::PdQuickGridDiscretization::getOverlapMap(int d) const
{
  switch (d) {
    case 1:
      return oneDimensionalOverlapMap;
      break;
    case 3:
      return threeDimensionalOverlapMap;
      break;
    default:
      TEST_FOR_EXCEPTION(true, Teuchos::Exceptions::InvalidParameter, 
                         std::endl << "PdQuickGridDiscretization::getOverlapMap(int d) only supports dimensions d=1 or d=3. Supplied dimension d=" << d << std::endl); 
    }
}

Teuchos::RCP<const Epetra_BlockMap>
PeridigmNS::PdQuickGridDiscretization::getBondMap() const
{
  return bondMap;
}

Teuchos::RCP<Epetra_Vector>
PeridigmNS::PdQuickGridDiscretization::getInitialX() const
{
  return initialX;
}

Teuchos::RCP<Epetra_Vector>
PeridigmNS::PdQuickGridDiscretization::getCellVolume() const
{
  return cellVolume;
}

Teuchos::RCP<PeridigmNS::NeighborhoodData> 
PeridigmNS::PdQuickGridDiscretization::getNeighborhoodData() const
{
  return neighborhoodData;
}

unsigned int
PeridigmNS::PdQuickGridDiscretization::getNumBonds() const
{
  return numBonds;
}

const Epetra_BlockMap PeridigmNS::getOverlap(int ndf, int numShared, int*shared, int numOwned,const  int* owned, const Epetra_Comm& comm){

	int numPoints = numShared+numOwned;
	UTILITIES::Array<int> ids(numPoints);
	int *ptr = ids.get();

	for(int j=0;j<numOwned;j++,ptr++)
		*ptr=owned[j];

	for(int j=0;j<numShared;j++,ptr++)
		*ptr=shared[j];

	return Epetra_BlockMap(-1,numPoints, ids.get(),ndf, 0,comm);

}

const Epetra_BlockMap PeridigmNS::PdQuickGridDiscretization::getOwnedMap(const Epetra_Comm& comm,const QUICKGRID::Data& gridData, int ndf) {
	int numShared=0;
	int *sharedPtr=NULL;
	int numOwned = gridData.numPoints;
	const int *ownedPtr = gridData.myGlobalIDs.get();
	return getOverlap(ndf, numShared,sharedPtr,numOwned,ownedPtr,comm);
}

const Epetra_BlockMap PeridigmNS::PdQuickGridDiscretization::getOverlapMap(const Epetra_Comm& comm,const QUICKGRID::Data& gridData, int ndf) {
	UTILITIES::Array<int> sharedGIDS = getSharedGlobalIds(gridData);
	std::tr1::shared_ptr<int> sharedPtr = sharedGIDS.get_shared_ptr();
	int numShared = sharedGIDS.get_size();
	int *shared = sharedPtr.get();
	int *owned = gridData.myGlobalIDs.get();
	int numOwned = gridData.numPoints;
	return getOverlap(ndf,numShared,shared,numOwned,owned,comm);
}

UTILITIES::Array<int> PeridigmNS::getSharedGlobalIds(const QUICKGRID::Data& gridData){
	std::set<int> ownedIds(gridData.myGlobalIDs.get(),gridData.myGlobalIDs.get()+gridData.numPoints);
	std::set<int> shared;
	int *neighPtr = gridData.neighborhoodPtr.get();
	int *neigh = gridData.neighborhood.get();
	std::set<int>::const_iterator ownedIdsEnd = ownedIds.end();
	for(int p=0;p<gridData.numPoints;p++){
		int ptr = neighPtr[p];
		int numNeigh = neigh[ptr];
		for(int n=1;n<=numNeigh;n++){
			int id = neigh[ptr+n];
			/*
			 * look for id in owned points
			 */
			 if(ownedIdsEnd == ownedIds.find(id)){
				 /*
				  * add this point to shared
				  */
				 shared.insert(id);
			 }
		}
	}

	// Copy set into shared ptr
	UTILITIES::Array<int> sharedGlobalIds(shared.size());
	int *sharedPtr = sharedGlobalIds.get();
	set<int>::iterator it;
	for ( it=shared.begin() ; it != shared.end(); it++, sharedPtr++ )
		*sharedPtr = *it;

	return sharedGlobalIds;
}

shared_ptr<int> PeridigmNS::getLocalOwnedIds(const QUICKGRID::Data& gridData, const Epetra_BlockMap& overlapMap){
	UTILITIES::Array<int> localIds(gridData.numPoints);
	int *lIds = localIds.get();
	int *end = localIds.get()+gridData.numPoints;
	int *gIds = gridData.myGlobalIDs.get();
	for(; lIds != end;lIds++, gIds++)
		*lIds = overlapMap.LID(*gIds);
	return localIds.get_shared_ptr();
}

shared_ptr<int> PeridigmNS::getLocalNeighborList(const QUICKGRID::Data& gridData, const Epetra_BlockMap& overlapMap){
	UTILITIES::Array<int> localNeighborList(gridData.sizeNeighborhoodList);
	int *localNeig = localNeighborList.get();
	int *neighPtr = gridData.neighborhoodPtr.get();
	int *neigh = gridData.neighborhood.get();
	for(int p=0;p<gridData.numPoints;p++){
		int ptr = neighPtr[p];
		int numNeigh = neigh[ptr];
		localNeig[ptr]=numNeigh;
		for(int n=1;n<=numNeigh;n++){
			int gid = neigh[ptr+n];
			int localId = overlapMap.LID(gid);
			localNeig[ptr+n] = localId;
		}
	}
	return localNeighborList.get_shared_ptr();
}


