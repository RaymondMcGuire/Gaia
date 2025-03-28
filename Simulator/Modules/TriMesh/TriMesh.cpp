#include "TriMesh.h"
#include "CuMatrix/Geometry/Geometry.h"
#include <MeshFrame/Parser/strutil.h>
#include <MeshFrame/Utility/IO.h>
#include <MeshFrame/Utility/STR.h>
#include "../Parallelization/CPUParallelization.h"

#include <MeshFrame/TriMesh/MeshStatic.h>

using namespace GAIA;

std::map<std::string, TriMeshTopology::SharedPtr> TriMeshFEM::topologies;
std::mutex TriMeshFEM::topologies_lock;

void  GAIA::TriMeshFEM::initialize(TriMeshParams::SharedPtr inObjectParams, bool precomputeToplogy)
{
	pObjectParams = inObjectParams;
	MF::IO::FileParts fp(inObjectParams->path);

	if (fp.ext == ".obj")
	{
		loadObj(pObjectParams->path);
	}
	else {
		std::cout << "Error!!! Unsupported file format: " << fp.ext << std::endl;
		std::exit(-1);
	}

	applyRotationScalingTranslation();

	positionsPrev.resize(3, numVertices());
	positionsPrev.setZero();
	velocities.resize(3, numVertices());
	velocities.setZero();
	velocitiesPrev.resize(3, numVertices());
	velocitiesPrev.setZero();

	faceNormals.resize(3, numFaces());

	if (pObjectParams->use3DRestpose)
	{
		computeRestposeTrianglesFrom3D();
	}
	else
	{
		computeRestposeTrianglesFrom2D();
	}

	if (pObjectParams->initialState != "")
	{
		MF::IO::FileParts fileParts(pObjectParams->initialState);
		if (fileParts.ext == ".obj")
		{
			TriMeshFEM tempMesh;
			tempMesh.loadObj(pObjectParams->initialState);

			assert(tempMesh.positions_.cols() == positions_.cols());
			positions_ = tempMesh.positions_;

			if (inObjectParams->scaleInitialState)
			{
				positions_.row(0) *= pObjectParams->scale(0);
				positions_.row(1) *= pObjectParams->scale(1);
				positions_.row(2) *= pObjectParams->scale(2);
			}
		}
		else if (fileParts.ext == ".json")
		{
			std::cout << "Error!!! Json initial state has not been implemented yet!" << std::endl;
			exit(-1);
		}
		else
		{
			std::cout << "Error!!! Unsupported file format: " << fileParts.ext << std::endl;
			exit(-1);
		}
	}

	if (precomputeToplogy)
	{
		computeTopology();
	}

	fixedMask.resize(numVertices());
	fixedMask.setZero();
	for (size_t iFixedV = 0; iFixedV < pObjectParams->fixedPoints.size(); iFixedV++)
	{
		fixedMask(pObjectParams->fixedPoints[iFixedV]) = true;
	}

	globalColors.resize(numVertices());
}

void GAIA::TriMeshFEM::applyRotationScalingTranslation()
{
	Eigen::AngleAxisf angleAxis;
	FloatingType rot = pObjectParams->rotation.norm();
	angleAxis.angle() = rot;
	angleAxis.axis() = pObjectParams->rotation / rot;

	// new intial state also need to be scaled & translated
	if (rot > CMP_EPSILON)
	{
		Eigen::Matrix3f R;
		R = angleAxis.toRotationMatrix();
		positions_ = R * positions_;
	}

	positions_.row(0) *= pObjectParams->scale(0);
	positions_.row(1) *= pObjectParams->scale(1);
	positions_.row(2) *= pObjectParams->scale(2);
	positions_.colwise() += pObjectParams->translation;
}

void GAIA::TriMeshFEM::evaluateFaceNormals(bool normalize)
{
	cpu_parallel_for(0, numFaces(), [&](int iFace) {
		faceNormals.col(iFace) = computeNormal(iFace, normalize);
		});
}

void GAIA::TriMeshFEM::computeTopology()
{
	// scope of topologyLockGuard lock
	bool alreadyComputed = false;
	{
		std::lock_guard<std::mutex> topologyLockGuard(topologies_lock);
		std::string modelPath = pObjectParams->path;

		auto pTopoItem = topologies.find(modelPath);
		if (pTopoItem == topologies.end())
		{
			pTopology = createTopology();
			topologies.insert({ modelPath, pTopology });
			alreadyComputed = false;
		}
		else
		{
			pTopology = pTopoItem->second;
			alreadyComputed = true;
		}
	}

	if (!alreadyComputed)
	{
		pTopology->initialize(this, pObjectParams.get());
	}
}

void GAIA::TriMeshFEM::get2DProjectionAxes(Vec3& normal, Eigen::Matrix<FloatingType, 3, 2>& axes)
{
	CuMatrix::buildOrthonormalBasis(normal.data(), axes.col(0).data(), axes.col(1).data());
}

void GAIA::TriMeshFEM::projectTriangleTo2D(int iF, const Eigen::Matrix<FloatingType, 3, 2>& axes, Eigen::Matrix<FloatingType, 2, 3>& tri2D)
{
	int32_t* faceVIds = facePos.col(iF).data();

	Eigen::Matrix<FloatingType, 2, 3> axesT = axes.transpose();

	tri2D.col(0) << 0.f, 0.f;
	Vec3 v1 = vertex(faceVIds[0]);

	for (int iV = 1; iV < 3; iV++)
	{
		// ptsPermuted3D.col(iV) = vertex(repermutedTetVIds[iV]);
		tri2D.col(iV) = axesT * (vertex(faceVIds[iV]) - v1);
	}
}


void GAIA::TriMeshFEM::computeDsFromPosition(int fId, Mat3x2& Ds)
{
	Vec3 v1 = positions_.col(facePos(1, fId)) - positions_.col(facePos(0, fId));
	Vec3 v2 = positions_.col(facePos(2, fId)) - positions_.col(facePos(0, fId));

	Ds.col(0) = v1;
	Ds.col(1) = v2;
}

void GAIA::TriMeshFEM::calculateDeformationGradient(int fId, Mat3x2& F)
{
	Vec3 v1 = positions_.col(facePos(1, fId)) - positions_.col(facePos(0, fId));
	Vec3 v2 = positions_.col(facePos(2, fId)) - positions_.col(facePos(0, fId));

	F.col(0) = v1;
	F.col(1) = v2;

	auto DmInv = getDmInv(fId);

	F = F * DmInv;
}

void GAIA::TriMeshFEM::computeRestposeTrianglesFrom3D()
{
	restposeTriangles2D.resize(numFaces());
	DmInvs.resize(4 * numFaces());

	vertexMass.resize(numVertices());
	vertexMass.setZero();
	vertexInvMass.resize(numVertices());
	faceRestposeArea.resize(numFaces());

	for (size_t iF = 0; iF < numFaces(); iF++)
	{
		Vec3 normal = computeNormal(iF);
		Eigen::Matrix<FloatingType, 3, 2> axes;
		get2DProjectionAxes(normal, axes);
		projectTriangleTo2D(iF, axes, restposeTriangles2D[iF]);

		// v1 is always (0,0)
		Mat2 Dm = restposeTriangles2D[iF].block<2, 2>(0, 1);

		faceRestposeArea(iF) = Dm.determinant() * 0.5f;
		for (size_t iV = 0; iV < 3; iV++)
		{
			vertexMass(facePosVId(iF, iV)) += pObjectParams->density * faceRestposeArea(iF) / 3.f;
		}

		auto DmInv = getDmInv(iF);
		DmInv = Dm.inverse();
	}

	vertexInvMass = vertexMass.cwiseInverse();
}

void GAIA::TriMeshFEM::computeRestposeTrianglesFrom2D()
{
	restposeTriangles2D.resize(numFaces());
	DmInvs.resize(4 * numFaces());

	for (size_t iF = 0; iF < numFaces(); iF++)
	{
		Vec2 v1 = UVs.col(faceUVs(0, iF)) * pObjectParams->UVScale;

		auto DmInv = getDmInv(iF);
		restposeTriangles2D[iF].col(0) = v1;
		restposeTriangles2D[iF].col(1) = UVs.col(faceUVs(1, iF)) * pObjectParams->UVScale;
		restposeTriangles2D[iF].col(2) = UVs.col(faceUVs(2, iF)) * pObjectParams->UVScale;

		DmInv.col(0) = restposeTriangles2D[iF].col(1) - v1;
		DmInv.col(0) = restposeTriangles2D[iF].col(2) - v1;

		faceRestposeArea(iF) = DmInv.determinant() * 0.5f;
		for (size_t iV = 0; iV < 3; iV++)
		{
			vertexMass(facePosVId(iF, iV)) += pObjectParams->density * faceRestposeArea(iF) / 3.f;
		}

		DmInv = DmInv.inverse();
	}
	vertexInvMass = vertexMass.cwiseInverse();

}

TriMeshTopology::SharedPtr GAIA::TriMeshFEM::createTopology()
{
	return std::make_shared<TriMeshTopology>();
}

void GAIA::TriMeshFEM::loadObj(std::string objFile)
{
	FILE* pFile;
	/*Open file*/
	if ((pFile = fopen(objFile.c_str(), "r")) == NULL) {
		printf("Error in opening file: %s!\n", objFile.c_str());
		return;
	}
	char lineBuffer[MAX_LINE_SIZE];

	bool with_uv = false;
	bool with_normal = false;

	std::vector<Vec3> positionsArr;
	std::vector<Vec2> uvsArr;

	std::vector<Vec3I> facePosArr;
	std::vector<Vec3I> faceUVsArr;

	while (true) {
		/*If get nothing, free buffer, break!*/
		if (!fgets(lineBuffer, MAX_LINE_SIZE, pFile)) {
			break;
		}

		strutil::Tokenizer stokenizer(lineBuffer, " \r\n");
		stokenizer.nextToken();
		const char* token;

		if (strcmp(stokenizer.getToken(), "v") == 0) {
			Vec3 v;

			for (int i = 0; i < 3; i++) {
				stokenizer.nextToken();
				token = stokenizer.getToken();
				v[i] = strutil::parseStringToFloat(token);
			}

			positionsArr.push_back(v);
			continue;
		}
		else if (strcmp(stokenizer.getToken(), "vt") == 0) {
			with_uv = true;
			Vec2 uv;
			for (int i = 0; i < 2; i++) {
				stokenizer.nextToken();
				token = stokenizer.getToken();
				uv[i] = strutil::parseStringToFloat(token);
			}
			uvsArr.push_back(uv);
			continue;
		}
		else if (strcmp(stokenizer.getToken(), "vn") == 0) {
			//with_normal = true;
			//Vec3 n;
			//for (int i = 0; i < 3; i++)
			//{
			//	stokenizer.nextToken();
			//	token = stokenizer.getToken();
			//	n[i] = strutil::parseStringToFloat(token);
			//}
			//normals.push_back(n);
			//continue;
		}

		else if (strcmp(stokenizer.getToken(), "f") == 0) {
			Vec3I facePosVIds;
			Vec3I faceUVIds;
			for (int i = 0; i < 3; i++)
			{
				stokenizer.nextToken();
				token = stokenizer.getToken();

				strutil::Tokenizer tokenizer(token, "/\\");

				int indices[3];
				int k = 0;
				while (tokenizer.nextToken() != strutil::StringOver)
				{
					token = tokenizer.getToken();
					indices[k] = strutil::parseStringToInt(token);
					k++;
				}

				facePosVIds[i] = indices[0] - 1;
				if (k >= 2) {
					faceUVIds[i] = indices[1] - 1;
				}
				//if (with_normal && vExample.hasNormal())
				//	v[i]->normal() = normals[indices[2] - 1];
			}

			facePosArr.push_back(facePosVIds);
			if (with_uv)
			{
				faceUVsArr.push_back(faceUVIds);
			}
		}
	}
	fclose(pFile);

	// pad 1 to be directly used as embree's buffer
	positions_.resize(3, positionsArr.size() + 1);
	numVertices_ = positionsArr.size();
	UVs.resize(2, uvsArr.size());
	for (size_t i = 0; i < positionsArr.size(); i++)
	{
		positions_.col(i) = positionsArr[i];
	}

	for (size_t i = 0; i < uvsArr.size(); i++)
	{
		UVs.col(i) = uvsArr[i];
	}

	facePos.resize(3, facePosArr.size());
	for (size_t iF = 0; iF < facePosArr.size(); iF++)
	{
		facePos.col(iF) = facePosArr[iF];
	}

	faceUVs.resize(3, faceUVsArr.size());

	for (size_t iF = 0; iF < faceUVsArr.size(); iF++)
	{
		faceUVs.col(iF) = faceUVsArr[iF];
	}
}

void GAIA::TriMeshFEM::saveAsPLY(std::string filePath)
{
	FILE* fp = fopen(filePath.c_str(), "w");

	fprintf(fp, "ply\nformat ascii 1.0\ncomment Mocap generated\nelement vertex %d\n", numVertices());
	fprintf(fp, "property float x\nproperty float y\nproperty float z\n");

	if (numFaces() != 0) {

		fprintf(fp, "element face %d \nproperty list uchar int vertex_indices\n", numFaces());
	}
	fprintf(fp, "end_header\n");

	//ss << ;
	//fprintf(fp, "%s", ss.str());

	for (int i = 0; i < numVertices(); i++)
	{
		// ss << verts[i][0] << " " << verts[i][1] << " " << verts[i][2] << "\n";
		fprintf(fp, "%f %f %f\n", positions_(0, i), positions_(1, i), positions_(2, i));
	}

	for (int i = 0; i < numFaces(); i++) {
		//ss << faces[i].size();
		//for (size_t j = 0; j < faces[i].size(); j++)
		//{
		//	ss << " " << faces[i][j];
		//}
		//ss << "\n";
		fprintf(fp, "3 %d %d %d\n", facePos(0, i), facePos(1, i), facePos(2, i));

	}


	////fputs("This is testing for fputs...\n", fp);
	fclose(fp);
}



bool GAIA::TriMeshParams::fromJson(nlohmann::json& objectParam)
{
	ObjectParams::fromJson(objectParam);
	EXTRACT_FROM_JSON(objectParam, use3DRestpose);
	EXTRACT_FROM_JSON(objectParam, triangleColoringCategoriesPath);
	EXTRACT_FROM_JSON(objectParam, initialState);
	EXTRACT_FROM_JSON(objectParam, scaleInitialState);
	EXTRACT_FROM_JSON(objectParam, frictionDynamic);
	EXTRACT_FROM_JSON(objectParam, frictionEpsV);
	EXTRACT_FROM_JSON(objectParam, bendingStiffness);
	return true;
}

bool GAIA::TriMeshParams::toJson(nlohmann::json& objectParam)
{
	ObjectParams::toJson(objectParam);
	PUT_TO_JSON(objectParam, use3DRestpose);
	PUT_TO_JSON(objectParam, triangleColoringCategoriesPath);
	PUT_TO_JSON(objectParam, initialState);
	PUT_TO_JSON(objectParam, scaleInitialState);
	PUT_TO_JSON(objectParam, frictionDynamic);
	PUT_TO_JSON(objectParam, frictionEpsV);
	PUT_TO_JSON(objectParam, bendingStiffness);
	return true;
}

void GAIA::TriMeshTopology::initialize(TriMeshFEM* pTriMesh, TriMeshParams* pObjectParams)
{
	MF::TriMesh::TriMeshStaticF::SharedPtr pMeshMF = std::make_shared<MF::TriMesh::TriMeshStaticF>();

	std::vector<std::array<double, 3>> verts;
	std::vector<std::array<int, 3>> faces;

	for (size_t iVert = 0; iVert < pTriMesh->numVertices(); iVert++)
	{
		verts.push_back({ pTriMesh->vertex(iVert)(0), pTriMesh->vertex(iVert)(1), pTriMesh->vertex(iVert)(2) });
	}

	for (size_t iFace = 0; iFace < pTriMesh->numFaces(); iFace++)
	{
		faces.push_back({ pTriMesh->facePos(0, iFace), pTriMesh->facePos(1, iFace), pTriMesh->facePos(2, iFace), });
	}

	// initialize MeshFrame::Trimesh
	pMeshMF->readVFList(&verts, &faces);

	// compute topology using MeshFrame::Trimesh

	// edge infos
	int iE = 0;
	for (MF::TriMesh::TriMeshStaticF::EPtr pE : MF::TriMesh::TriMeshStaticIteratorF::MEIterator(pMeshMF.get()))
	{
		assert(iE == pE->index());
		EdgeInfo eInfo;
		eInfo.eV1 = pE->halfedge()->source()->id();
		eInfo.eV2 = pE->halfedge()->target()->id();
		eInfo.fId1 = pE->halfedge()->face()->id();
		eInfo.eV12Next = pE->halfedge()->he_next()->target()->id();

		eInfo.eV1FaceOrder[0] = -1;
		eInfo.eV2FaceOrder[0] = -1;
		eInfo.eV1FaceOrder[1] = -1;
		eInfo.eV2FaceOrder[1] = -1;

		for (int iFV = 0; iFV < 3; iFV++)
		{
			if (pTriMesh->facePosVId(eInfo.fId1, iFV) == eInfo.eV1)
			{
				eInfo.eV1FaceOrder[0] = iFV;
			}

			if (pTriMesh->facePosVId(eInfo.fId1, iFV) == eInfo.eV2)
			{
				eInfo.eV2FaceOrder[0] = iFV;
			}
		}
		assert(eInfo.eV1FaceOrder[0] != -1 && eInfo.eV2FaceOrder[0] != -1);

		if (!pE->boundary())
		{
			eInfo.eV21Next = pE->halfedge()->he_sym()->he_next()->target()->id();
			eInfo.fId2 = pE->halfedge()->he_sym()->face()->id();
			for (int iFV = 0; iFV < 3; iFV++)
			{
				if (pTriMesh->facePosVId(eInfo.fId2, iFV) == eInfo.eV1)
				{
					eInfo.eV1FaceOrder[1] = iFV;
				}

				if (pTriMesh->facePosVId(eInfo.fId2, iFV) == eInfo.eV2)
				{
					eInfo.eV2FaceOrder[1] = iFV;
				}
			}
			assert(eInfo.eV1FaceOrder[1] != -1 && eInfo.eV2FaceOrder[1] != -1);
		}
		else
		{
			eInfo.eV21Next = -1;
			eInfo.fId2 = -1;
		}

		edgeInfos.push_back(eInfo);
		iE++;
	}
	numEdges = edgeInfos.size();

	// face neighbor faces
	faces3NeighborFaces.resize(3, pTriMesh->numFaces());
	faces3NeighborEdges.resize(3, pTriMesh->numFaces());
	int iF = 0;
	for (MF::TriMesh::TriMeshStaticF::FPtr pSurfF : MF::TriMesh::TriMeshStaticIteratorF::MFIterator(pMeshMF.get()))
	{
		MF::TriMesh::TriMeshStaticF::HEPtr pHE_start = nullptr;
		for (MF::TriMesh::TriMeshStaticF::HEPtr pHE : MF::TriMesh::TriMeshStaticIteratorF::FHEIterator(pSurfF))
		{
			// find the half edge that matches the first edge of the face
			if (pHE->source()->id() == pTriMesh->facePosVId(iF, 0) && pHE->target()->id() == pTriMesh->facePosVId(iF, 1))
			{
				pHE_start = pHE;
				break;
			}
		}
		// say 3 vertices in surfaceFacesTetMeshVIds are A, B and C,
		// the 3 edges will be AB, BC, CD
		// and 3 neighbor faces will be on three face on the other side of AB, BC, CA correspondingly
		// this gurrantees that
		for (size_t iV = 0; iV < 3; iV++)
		{
			MF::TriMesh::TriMeshStaticF::HEPtr pHE_dual = MF::TriMesh::TriMeshStaticF::halfedgeSym(pHE_start);

			int iVNext = (iV + 1) % 3;
			assert(pHE_start->source()->id() == pTriMesh->facePosVId(iF, iV) && pHE_start->target()->id() == pTriMesh->facePosVId(iF, iVNext));
			//std::cout << "Face " << iF << " Edge " << iV << ": " << pHE_start->source()->id() << ", " << pHE_start->target()->id() << std::endl;
			faces3NeighborEdges(iV, iF) = pHE_start->edge()->index();
			if (pHE_dual != nullptr)
			{
				faces3NeighborFaces(iV, iF) = pHE_dual->face()->id();
			}
			else
			{
				faces3NeighborFaces(iV, iF) = -1;
			}

			pHE_start = MF::TriMesh::TriMeshStaticF::halfedgeNext(pHE_start);
		}

		++iF;
	}

	// vertex neighbors
	int iV = 0;
	std::vector<IdType> vertexNeighborFaces_;
	std::vector<IdType> vertexNeighborVertices_;
	std::vector<IdType> vertexNeighborEdges_;
	std::vector<IdType> vertexNeighborFaces_vertexOrder_;
	std::vector<IdType> vertexNeighborEdges_vertexOrder_;

	vertexNeighborFaces_infos.resize(pTriMesh->numVertices() * 2);
	vertexNeighborVertices_infos.resize(pTriMesh->numVertices() * 2);
	vertexNeighborEdges_infos.resize(pTriMesh->numVertices() * 2);

	std::vector<IdType> vertexRelevantBendings_;
	std::vector<IdType> vertexRelevantBendings_vertexOrder_;
	vertexRelevantBendings_infos.resize(pTriMesh->numVertices() * 2);

	for (MF::TriMesh::TriMeshStaticF::VPtr pV : MF::TriMesh::TriMeshStaticIteratorF::MVIterator(pMeshMF.get()))
	{
		size_t numNeiVerteices = 0, numNeiFaces = 0, numNeiEdges = 0, numRelevantBendings = 0;
		vertexNeighborVertices_infos(iV * 2) = vertexNeighborVertices_.size();
		for (MF::TriMesh::TriMeshStaticF::VPtr pVNei : MF::TriMesh::TriMeshStaticIteratorF::VVIterator(pV)) {
			++numNeiVerteices;
			vertexNeighborVertices_.push_back(pVNei->id());
		}
		vertexNeighborVertices_infos(iV * 2 + 1) = numNeiVerteices;

		vertexNeighborFaces_infos(iV * 2) = vertexNeighborFaces_.size();
		for (MF::TriMesh::TriMeshStaticF::FPtr pNeiF : MF::TriMesh::TriMeshStaticIteratorF::VFIterator(pV)) {
			numNeiFaces++;
			vertexNeighborFaces_.push_back(pNeiF->id());

			// find the order of the vertex in the face
			IdType iVInF = -1;
			IdType iFV = 0;
			for (MF::TriMesh::TriMeshStaticF::VPtr pFV : MF::TriMesh::TriMeshStaticIteratorF::FVIterator(pNeiF))
			{
				if (pFV->id() == iV) { iVInF = iFV;	break; }
				++iFV;
			}
			assert(iVInF != -1);
			vertexNeighborFaces_vertexOrder_.push_back(iVInF);
		}
		vertexNeighborFaces_infos(iV * 2 + 1) = numNeiFaces;

		vertexNeighborEdges_infos(iV * 2) = vertexNeighborEdges_.size();
		vertexRelevantBendings_infos(iV * 2) = vertexRelevantBendings_.size();
		for (MF::TriMesh::TriMeshStaticF::EPtr pNeiE : MF::TriMesh::TriMeshStaticIteratorF::VEIterator(pV))
		{
			numNeiEdges++;
			vertexNeighborEdges_.push_back(pNeiE->id());

			if (edgeInfos[pNeiE->id()].eV1 == pV->id())
			{
				vertexNeighborEdges_vertexOrder_.push_back(0);
			}
			else
			{
				vertexNeighborEdges_vertexOrder_.push_back(1);
			}

			if (!pNeiE->boundary())
			{
				numRelevantBendings++;
				// relevant bendings where vertex is on the edge
				vertexRelevantBendings_.push_back(pNeiE->id());

				if (edgeInfos[pNeiE->id()].eV1 == pV->id())
				{
					vertexRelevantBendings_vertexOrder_.push_back(0);
				}
				else
				{
					vertexRelevantBendings_vertexOrder_.push_back(1);
				}
			}
		}
		// relevant bendings where vertex is not on the edge, but cross it
		for (MF::TriMesh::TriMeshStaticF::FPtr pNeiF : MF::TriMesh::TriMeshStaticIteratorF::VFIterator(pV))
		{
			bool crossEdgeFound = false;
			for (MF::TriMesh::TriMeshStaticF::EPtr pFE : MF::TriMesh::TriMeshStaticIteratorF::FEIterator(pNeiF))
			{
				if (pFE->boundary())
				{
					crossEdgeFound = true;
					continue;
				}
				// find the edge that cross vertex
				// order of edge is corresponds to its 1st halfedge
				// eInfo.eV1 = pE->halfedge()->source()->id();
				// eInfo.eV2 = pE->halfedge()->target()->id();
				MF::TriMesh::TriMeshStaticF::HEPtr pHE = MF::TriMesh::TriMeshStaticF::edgeHalfedge(pFE);

				if (pHE->source()->id() != pV->id() && pHE->target()->id() != pV->id())
				{
					crossEdgeFound = true;
					numRelevantBendings++;
					vertexRelevantBendings_.push_back(pFE->id());
					if (pHE->face() == pNeiF)
						// eV12Next
					{
						vertexRelevantBendings_vertexOrder_.push_back(2);
					}
					else
						// eV21Next
					{
						vertexRelevantBendings_vertexOrder_.push_back(3);
					}
					break;
				}
			}
			assert(crossEdgeFound);
		}

		vertexNeighborEdges_infos(iV * 2 + 1) = numNeiEdges;
		vertexRelevantBendings_infos(iV * 2 + 1) = numRelevantBendings;

		iV++;
	}

	//std::cout << "vertexNeighborFaces_infos:\n" << vertexNeighborFaces_infos;
	//std::cout << "vertexNeighborVertices_infos:\n" << vertexNeighborVertices_infos;

	vertexNeighborFaces = VecDynamicI::Map(&vertexNeighborFaces_[0], vertexNeighborFaces_.size());
	vertexNeighborFaces_vertexOrder = VecDynamicI::Map(&vertexNeighborFaces_vertexOrder_[0], vertexNeighborFaces_vertexOrder_.size());
	vertexNeighborVertices = VecDynamicI::Map(&vertexNeighborVertices_[0], vertexNeighborVertices_.size());
	vertexNeighborEdges = VecDynamicI::Map(&vertexNeighborEdges_[0], vertexNeighborEdges_.size());
	vertexNeighborEdges_vertexOrder = VecDynamicI::Map(&vertexNeighborEdges_vertexOrder_[0], vertexNeighborEdges_vertexOrder_.size());

	vertexRelevantBendings = VecDynamicI::Map(&vertexRelevantBendings_[0], vertexRelevantBendings_.size());
	vertexRelevantBendings_vertexOrder = VecDynamicI::Map(&vertexRelevantBendings_vertexOrder_[0], vertexRelevantBendings_vertexOrder_.size());

	//std::cout << "vertexRelevantBendings:\n" << vertexRelevantBendings;
	//std::cout << "vertexRelevantBendings_infos:\n" << vertexRelevantBendings_infos;
	//std::cout << "vertexRelevantBendings_vertexOrder:\n" << vertexRelevantBendings_vertexOrder;

	// load vertices coloring information
	if (pObjectParams->verticesColoringCategoriesPath != "")
	{
		nlohmann::json vertsColoring;

		MF::loadJson(pObjectParams->verticesColoringCategoriesPath, vertsColoring);
		MF::convertJsonParameters(vertsColoring, verticesColoringCategories);

		// we sort them from large to small for the aggregated solve
		std::sort(verticesColoringCategories.begin(), verticesColoringCategories.end(),
			[](const std::vector<int>& a, const std::vector<int>& b) { return a.size() > b.size(); });
	}
}
