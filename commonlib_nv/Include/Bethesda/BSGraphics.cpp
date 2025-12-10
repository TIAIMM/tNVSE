#include "BSGraphics.hpp"

bool* const BSGraphics::bOcclusionQueryActive = (bool*)0x12030A4;
bool* const BSGraphics::bLetterBox = (bool*)0x11F9426;

bool BSGraphics::IsLetterBox() {
	return *bLetterBox;
}

BSGraphics::OcclusionQuery::OcclusionQuery() {
	pOcclusionQuery = nullptr;
	NiDX9Renderer::GetSingleton()->m_pkD3DDevice9->CreateQuery(D3DQUERYTYPE_OCCLUSION, &pOcclusionQuery);
}

BSGraphics::OcclusionQuery::~OcclusionQuery() {
	if (pOcclusionQuery)
		pOcclusionQuery->Release();
}

bool BSGraphics::OcclusionQuery::Begin() {
	if (*BSGraphics::bOcclusionQueryActive)
		return false;
	*BSGraphics::bOcclusionQueryActive = true;
	pOcclusionQuery->Issue(D3DISSUE_BEGIN);
	return true;
}

void BSGraphics::OcclusionQuery::End() {
	if (*BSGraphics::bOcclusionQueryActive) {
		*BSGraphics::bOcclusionQueryActive = false;
		pOcclusionQuery->Issue(D3DISSUE_END);
	}
}

bool BSGraphics::OcclusionQuery::GetOcclusionQueryResults(void* apData, bool abFlush) {
	return SUCCEEDED(pOcclusionQuery->GetData(apData, sizeof(DWORD), abFlush));
}

bool BSGraphics::OcclusionQuery::GetData(void* apData, DWORD adwGetDataFlags) {
	return SUCCEEDED(pOcclusionQuery->GetData(apData, sizeof(DWORD), adwGetDataFlags));
}
