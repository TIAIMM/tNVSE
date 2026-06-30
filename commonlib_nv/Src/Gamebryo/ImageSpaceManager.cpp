#include "ImageSpaceManager.hpp"
#include "AutoMemContext.hpp"
#include "BSTextureManager.hpp"


// GAME - 0xB8B3F0
void ImageSpaceManager::PrepareEffectRange() {
	ThisStdCall(0xB8B3F0, this);
}

ImageSpaceShaderParam& ImageSpaceManager::GetDefaultParam() {
	return *(ImageSpaceShaderParam*)0x1202410;
}

// Source texture

// GAME - 0xB8C980
void ImageSpaceManager::RenderEffect(ImageSpaceEffect * apEffect, NiDX9Renderer * apRenderer, NiSourceTexture * apSourceTarget, BSRenderedTexture * apDestTarget, ImageSpaceEffectParam* apParam, bool abEndFrame) {
	ThisStdCall(0xB8C980, this, apEffect, apRenderer, apSourceTarget, apDestTarget, apParam, abEndFrame);
}

// GAME - 0xB975F0
void ImageSpaceManager::RenderEffect(ImageSpaceManager::EffectID aeID, NiDX9Renderer * apRenderer, NiSourceTexture * apSourceTarget, BSRenderedTexture * apDestTarget, ImageSpaceEffectParam* apParam, bool abEndFrame) {
	RenderEffect(kEffects.m_pBase[aeID], apRenderer, apSourceTarget, apDestTarget, apParam, abEndFrame);
}


// Rendered texture

// GAME - 0xB8C830
void ImageSpaceManager::RenderEffect(ImageSpaceEffect* apEffect, NiDX9Renderer* apRenderer, BSRenderedTexture* apSourceTarget, BSRenderedTexture* apDestTarget, ImageSpaceEffectParam* apParam, bool abEndFrame) {
	ThisStdCall(0xB8C830, this, apEffect, apRenderer, apSourceTarget, apDestTarget, apParam, abEndFrame);
}

// GAME - 0xB97550
void ImageSpaceManager::RenderEffect(ImageSpaceManager::EffectID aeID, NiDX9Renderer* apRenderer, BSRenderedTexture* apSourceTarget, BSRenderedTexture* apDestTarget, ImageSpaceEffectParam* apParam, bool abEndFrame) {
	ImageSpaceManager::RenderEffect(kEffects.m_pBase[aeID], apRenderer, apSourceTarget, apDestTarget, apParam, abEndFrame);
}


// GAME - 0xB8C730
void ImageSpaceManager::RenderEffect(ImageSpaceManager::EffectID aeID, NiDX9Renderer* pkRenderer, BSRenderedTexture* apDestTarget, ImageSpaceEffectParam* apParam, bool abEndFrame) {
	ThisStdCall(0xB8C730, this, aeID, pkRenderer, apDestTarget, apParam, abEndFrame);
}


// GAME - 0xB97900
void ImageSpaceManager::RenderEffectRange(NiDX9Renderer* apRenderer, BSRenderedTexture*& aprSourceTarget, BSRenderedTexture*& aprDestTarget) {
	ThisStdCall(0xB97900, this, apRenderer, aprSourceTarget, aprDestTarget);
}