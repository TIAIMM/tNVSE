#pragma once
#include <cstring>
#include "SafeWrite.h"
#include "Game/uidecode.h"
#include "tnvse.h"

namespace tNVSE {
    static void __cdecl ConvertToAsciiQuotes(UInt8* currentChar) {
        return CdeclCall<void>(0xA122B0);
    }

    static bool __cdecl ReplaceVariableInString(const char* varName, char* outBuffer, size_t bufferSize, bool isPositiveEscape) {
        return CdeclCall<bool>(0x7070C0);
    }

    static bool __cdecl ParseAndFormatVariableInString(const char* p_varNameBuffer, void* p_parsedTextBuffer) {
        return CdeclCall<bool>(0x7073D0);
    }

    static SInt32 __cdecl SafeFormatString(char* buffer, const char* format, ...) {
        return CdeclCall<SInt32>(0xEC623A);
    }

    static SInt32 __cdecl SafeStringCopy(char* pDest, size_t nDestSize, const char* pSrc) {
        return CdeclCall<SInt32>(0xEC65A6);
    }

    static SInt32 __cdecl SafeFormat(char* pDest, size_t nDestSize, const char* pFormat, ...) {
        return CdeclCall<SInt32>(0x406D00);
    }

    static SInt32 __cdecl AlignLineWidthToTab(double a1, double a2) {
        return CdeclCall<SInt32>(0xEC9130);
    }

    static UInt32 __cdecl OptimizedMemCopy(UInt32 destAddress, UInt8* srcData, UInt32 byteCount) {
        return CdeclCall<UInt32>(0xEC62C0);
    }

    static void* __cdecl OptimizedMemSet(void* buf_1, int a2, size_t n0x100) {
        return CdeclCall<void*>(0xEC61C0);
    }

    static bool __cdecl StringBuffer_Manage(void* StringBuffer, const char* srcString, size_t minRequiredSize) {
        return ThisStdCall<bool>(0x4037F0, StringBuffer, srcString, minRequiredSize);
    }

    static void* __cdecl AllocateMemAligned(void* pMemoryManager, unsigned int size) {
        return ThisStdCall<void*>(0xAA3E40, pMemoryManager, size);
    }

    static void* __cdecl ReallocateMem(void* pMemoryManager, void* pOldBlock, SIZE_T nNewSize) {
        return ThisStdCall<void*>(0xAA4150, pMemoryManager, pOldBlock, nNewSize);
    }

    static void* __cdecl DeallocMem(void* pMemoryManager, void* pMemoryBlock) {
        return ThisStdCall<void*>(0xAA4060, pMemoryManager, pMemoryBlock);
    }

    static void* __cdecl AppendToListTail(void* ListNode, void * ListNode2) {
        return ThisStdCall<void*>(0xAF25DD, ListNode, ListNode2);
    }

    static __declspec(naked) UInt32 __cdecl
        ConditionalFloatToUInt(double a1)
    {
        __asm {
            fld qword ptr[esp + 4]
            jmp dword ptr ds : 0EC62C0h
        }
    }

    __declspec(naked) void __cdecl FastStringCopyAligned(char* dest, const char* src)
    {
        __asm {
            push    edi
            mov     edi, [esp + 8]
            mov     eax, 0xEC63E5
            jmp     eax
        }
    }

    static Float32 __stdcall VertSpacingAdjust(UInt32 fontID) {
        return 0;
    }

    static inline bool IsGB2312LeadByte(unsigned char c) {
        return (c >= 0xA1 && c <= 0xF7);
    }

    inline NiVector3& StringDefaulDimensions = *reinterpret_cast<NiVector3*>(0x11F426C);

    class FontInfoEx : public FontInfo {
    public:
        void __thiscall ProcessTextLayoutEx(char* textSrc, FontTextReplaced* textParams) {
            unsigned int charWidthWithKerning; // eax
            unsigned int spaceCharWidth; // eax
            unsigned int tildeCharWidth; // eax
            unsigned int hyphenCharWidth; // eax
            unsigned int nextCharWidth; // eax
            unsigned int hyphenInsertWidth; // eax
            unsigned int combinedCharWidth; // eax
            int finalMaxLineWidth; // [esp+0h] [ebp-7E0h]
            int tempLineWidthComp1; // [esp+4h] [ebp-7DCh]
            int tempLineWidthComp2; // [esp+8h] [ebp-7D8h]
            int tempLineWidthComp3; // [esp+Ch] [ebp-7D4h]
            int tempLineWidthComp4; // [esp+10h] [ebp-7D0h]
            int escapeSeqEffectiveLen; // [esp+14h] [ebp-7CCh]
            char* processedTextBuffer; // [esp+8Ch] [ebp-754h]
            char* originalTextBuffer; // [esp+94h] [ebp-74Ch]
            unsigned int truncateCharCounter; // [esp+9Ch] [ebp-744h]
            UInt32 lineCounter; // [esp+A0h] [ebp-740h]
            unsigned int truncatedTextLen; // [esp+A4h] [ebp-73Ch]
            unsigned __int8 currentChar; // [esp+ABh] [ebp-735h] BYREF
            FontInfo::GlyphInfo* pCurrentGlyph; // [esp+ACh] [ebp-734h]
            size_t charIndex; // [esp+B0h] [ebp-730h]
            size_t bufferCopyIndex; // [esp+B4h] [ebp-72Ch]
            char textureNameBuffer[268]; // [esp+B8h] [ebp-728h] BYREF
            int charScanIndex; // [esp+1C4h] [ebp-61Ch]
            float unkarray[4]; // [esp+1D8h] [ebp-608h]
            char substrBuffer[264]; // [esp+1E8h] [ebp-5F8h] BYREF
            signed int escapeSeqSizeDiff; // [esp+2F0h] [ebp-4F0h]
            size_t postEscapeTextLen; // [esp+2F4h] [ebp-4ECh]
            bool isPositiveEscape; // [esp+2FBh] [ebp-4E5h]
            int escapeSeqPrefixLen; // [esp+2FCh] [ebp-4E4h]
            size_t totalEscapeSeqLen; // [esp+300h] [ebp-4E0h]
            int varNameLen; // [esp+304h] [ebp-4DCh]
            char varNameBuffer[33]; // [esp+308h] [ebp-4D8h] BYREF
            unsigned int srcTextIndex; // [esp+38Ch] [ebp-454h]
            signed int currentLineWidth; // [esp+390h] [ebp-450h] BYREF
            unsigned int processedTextLen; // [esp+394h] [ebp-44Ch]
            unsigned int lastWrapPosition; // [esp+398h] [ebp-448h]
            int maxLineWidth; // [esp+39Ch] [ebp-444h]
            char* dynamicTextBuffer; // [esp+3A0h] [ebp-440h]
            UInt32 buttonIconIndex; // [esp+3A4h] [ebp-43Ch]
            int preSpaceWidth; // [esp+3A8h] [ebp-438h] BYREF
            bool hasEscapeSequence; // [esp+3AFh] [ebp-431h]
            signed int postSpaceWidth; // [esp+3B0h] [ebp-430h]
            bool isTildeChar; // [esp+3B7h] [ebp-429h]
            char* processedOriginalText; // [esp+3B8h] [ebp-428h]
            int currentLineCount; // [esp+3BCh] [ebp-424h]
            size_t textBufferSize; // [esp+3C0h] [ebp-420h]
            int hyphenInsertCount; // [esp+3C4h] [ebp-41Ch]
            char parsedTextBuffer[1028]; // [esp+3C8h] [ebp-418h] BYREF
            float lineSpacingAdjust; // [esp+7D0h] [ebp-10h]
            float totalTextHeight; // [esp+7D4h] [ebp-Ch]
            size_t sourceTextLen; // [esp+7D8h] [ebp-8h]
            int maxAllowedLines; // [esp+7DCh] [ebp-4h]

            if (!textSrc)
                return;
            if (textParams->wrapWidth <= 0)
                textParams->wrapWidth = 0x7FFFFFFF;
            if (textParams->wrapLimit <= 0)
                textParams->wrapLimit = 0x7FFFFFFF;
            if (textParams->wrapLines <= 0)
                textParams->wrapLines = 0x7FFFFFFF;
            lineSpacingAdjust = VertSpacingAdjust(this->fontID);
            lastWrapPosition = 0;
            preSpaceWidth = 0;
            postSpaceWidth = 0;
            currentLineWidth = 0;
            maxLineWidth = 0;
            totalTextHeight = this->fontData->glyphs[' '].height;
            currentLineCount = 1;
            sourceTextLen = strlen(textSrc);
            maxAllowedLines = textParams->wrapLines;
            originalTextBuffer = static_cast<char*>(AllocateMemAligned(reinterpret_cast<void*>(0x11F6238), sourceTextLen + 4));
            OptimizedMemSet(originalTextBuffer, 0, sourceTextLen + 4);
            processedOriginalText = originalTextBuffer;
            processedTextBuffer = static_cast<char*>(AllocateMemAligned(reinterpret_cast<void*>(0x11F6238), sourceTextLen + 4));
            OptimizedMemSet(processedTextBuffer, 0, sourceTextLen + 4);
            dynamicTextBuffer = processedTextBuffer;
            SafeFormatString(originalTextBuffer, "%s", textSrc);
            processedTextLen = 0;
            textBufferSize = sourceTextLen + 4;
            hyphenInsertCount = 0;
            isTildeChar = 0;
            parsedTextBuffer[0] = 0;
            hasEscapeSequence = 0;
            for (srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
            {
                if (processedOriginalText[srcTextIndex] == '&')
                {
                    varNameLen = 0;
                    escapeSeqPrefixLen = 1;
                    isPositiveEscape = 1;
                    if (processedOriginalText[srcTextIndex + 1] == '-')
                    {
                        isPositiveEscape = 0;
                        escapeSeqPrefixLen = 2;
                    }
                    while (processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex]
                        && varNameLen < 127
                        && processedOriginalText[varNameLen + srcTextIndex] != ';'
                        && processedOriginalText[varNameLen + srcTextIndex] != '\n'
                        && processedOriginalText[varNameLen + srcTextIndex] != textParams->newLineCharacter)
                    {
                        varNameBuffer[varNameLen] = processedOriginalText[escapeSeqPrefixLen + varNameLen + srcTextIndex];
                        ++varNameLen;
                    }
                    if (varNameLen)
                        escapeSeqEffectiveLen = varNameLen - escapeSeqPrefixLen;
                    else
                        escapeSeqEffectiveLen = 0;
                    varNameBuffer[escapeSeqEffectiveLen] = 0;
                    totalEscapeSeqLen = (strlen(varNameBuffer) + 1);
                    if (processedOriginalText[varNameLen + srcTextIndex] == ';')
                        totalEscapeSeqLen += escapeSeqPrefixLen;
                    if (ReplaceVariableInString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
                        || ParseAndFormatVariableInString(varNameBuffer, parsedTextBuffer))
                    {
                        postEscapeTextLen = strlen(parsedTextBuffer);
                        escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;
                        if (parsedTextBuffer[postEscapeTextLen - 1] == '\\')
                        {
                            unkarray[0] = 0.0;
                            unkarray[1] = 0.0;
                            unkarray[2] = 0.0;
                            unkarray[3] = 0.0;
                            for (charScanIndex = 0; parsedTextBuffer[charScanIndex] != '\\'; ++charScanIndex)
                                ;
                            substrBuffer[0] = 0;
                            FastStringCopyAligned(&parsedTextBuffer[charScanIndex + 1], substrBuffer);
                            size_t strLen = strlen(substrBuffer);
                            *((char*)unkarray + strLen + 15) = 0;
                            if (this->fontID == 7)
                            {
                                SafeStringCopy(textureNameBuffer, 260, substrBuffer);
                                SafeFormat(substrBuffer, 260, "glow_%s", textureNameBuffer);
                            }
                            FontInfo::LoadFontIcon(substrBuffer);
                            parsedTextBuffer[charScanIndex] = 1;
                            parsedTextBuffer[charScanIndex + 1] = 0;
                            postEscapeTextLen = strlen(parsedTextBuffer);
                            escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;
                        }
                        if (escapeSeqSizeDiff > 0)
                        {
                            textBufferSize += escapeSeqSizeDiff;
                            dynamicTextBuffer = static_cast<char*>(ReallocateMem(reinterpret_cast<void*>(0x11F6238), dynamicTextBuffer, textBufferSize));
                        }
                        for (bufferCopyIndex = 0; bufferCopyIndex < postEscapeTextLen; ++bufferCopyIndex)
                            dynamicTextBuffer[processedTextLen++] = parsedTextBuffer[bufferCopyIndex];
                        srcTextIndex = srcTextIndex + totalEscapeSeqLen - 1;
                    }
                    else
                    {
                        dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
                    }
                    hasEscapeSequence = 1;
                }
                else
                {
                    dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
                }
            }
            dynamicTextBuffer[processedTextLen] = 0;
            if (hasEscapeSequence)
            {
                sourceTextLen = processedTextLen;
                processedOriginalText = static_cast<char*>(ReallocateMem(reinterpret_cast<void*>(0x11F6238), processedOriginalText, processedTextLen + 4));
                FastStringCopyAligned(processedOriginalText, dynamicTextBuffer);
            }
            *dynamicTextBuffer = 0;
            processedTextLen = 0;
            buttonIconIndex = 0;
            for (charIndex = 0; charIndex < sourceTextLen && processedOriginalText[charIndex]; ++charIndex)
            {
                if (processedOriginalText[charIndex] == textParams->newLineCharacter)
                {
                    dynamicTextBuffer[processedTextLen] = textParams->newLineCharacter;
                    if (++processedTextLen >= textBufferSize)
                    {
                        dynamicTextBuffer = static_cast<char*>(ReallocateMem(reinterpret_cast<void*>(0x11F6238), dynamicTextBuffer, processedTextLen + 4));
                        textBufferSize = processedTextLen + 4;
                    }
                    totalTextHeight = this->fontData->lineHeight + lineSpacingAdjust + totalTextHeight;
                    AppendToListTail(&textParams->lineWidths.m_listHead.data, &currentLineWidth);
                    if (maxLineWidth <= currentLineWidth)
                        tempLineWidthComp4 = currentLineWidth;
                    else
                        tempLineWidthComp4 = maxLineWidth;
                    maxLineWidth = tempLineWidthComp4;
                    currentLineWidth = 0;
                    lastWrapPosition = 0;
                    ++currentLineCount;
                }
                else
                {
                    if (processedOriginalText[charIndex] == '\t')
                    {
                        currentLineWidth += 75 - currentLineWidth % 75;
                        continue;
                    }
                    currentChar = processedOriginalText[charIndex];
                    ConvertToAsciiQuotes(&currentChar);
                    pCurrentGlyph = &this->fontData->glyphs[currentChar];
                    if (currentChar == 1)
                    {
                        if (buttonIconIndex < this->buttonIconArray.size)
                        {
                            pCurrentGlyph->width = this->buttonIconArray.data[buttonIconIndex].unk01;
                            pCurrentGlyph->kerningRight = this->buttonIconArray.data[buttonIconIndex].unk04;
                        }
                        ++buttonIconIndex;
                    }
                    charWidthWithKerning = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                    currentLineWidth += charWidthWithKerning;
                    if (currentChar == ' ')
                    {
                        lastWrapPosition = processedTextLen;
                        spaceCharWidth = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                        preSpaceWidth = currentLineWidth - spaceCharWidth;
                        postSpaceWidth = currentLineWidth;
                        isTildeChar = 0;
                    }
                    else if (currentChar == '~')
                    {
                        lastWrapPosition = processedTextLen;
                        preSpaceWidth = currentLineWidth;
                        postSpaceWidth = currentLineWidth;
                        isTildeChar = 1;
                        tildeCharWidth = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                        currentLineWidth -= tildeCharWidth;
                    }
                    if (currentLineWidth > textParams->wrapWidth)
                    {
                        if (lastWrapPosition)
                        {
                            if (isTildeChar)
                            {
                                isTildeChar = 0;
                                textBufferSize += 4;
                                dynamicTextBuffer = static_cast<char*>(ReallocateMem(reinterpret_cast<void*>(0x11F6238), dynamicTextBuffer, textBufferSize));
                                OptimizedMemCopy(
                                    reinterpret_cast<unsigned int>(&dynamicTextBuffer[lastWrapPosition + 2]),
                                    reinterpret_cast<unsigned __int8*>(&dynamicTextBuffer[lastWrapPosition]),
                                    processedTextLen - lastWrapPosition);
                                dynamicTextBuffer[lastWrapPosition + 1] = '\n';
                                dynamicTextBuffer[lastWrapPosition] = '-';
                                processedTextLen += 2;
                                hyphenInsertCount += 2;
                                pCurrentGlyph = &this->fontData->glyphs['-'];
                                totalTextHeight = this->fontData->lineHeight + lineSpacingAdjust + totalTextHeight;
                                hyphenCharWidth = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                                currentLineWidth -= hyphenCharWidth;
                                AppendToListTail(&textParams->lineWidths.m_listHead.data, &currentLineWidth);
                                if (maxLineWidth <= currentLineWidth)
                                    tempLineWidthComp3 = currentLineWidth;
                                else
                                    tempLineWidthComp3 = maxLineWidth;
                                maxLineWidth = tempLineWidthComp3;
                                lastWrapPosition = 0;
                                ++currentLineCount;
                                pCurrentGlyph = &this->fontData->glyphs[*(dynamicTextBuffer - 1)];
                                currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                                pCurrentGlyph = &this->fontData->glyphs[currentChar];
                                nextCharWidth = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                                currentLineWidth += nextCharWidth;
                            }
                            else
                            {
                                if (lastWrapPosition == processedTextLen)
                                    currentChar = textParams->newLineCharacter;
                                else
                                    dynamicTextBuffer[lastWrapPosition] = textParams->newLineCharacter;
                                totalTextHeight = this->fontData->lineHeight + lineSpacingAdjust + totalTextHeight;
                                AppendToListTail(&textParams->lineWidths.m_listHead.data, &preSpaceWidth);
                                if (maxLineWidth <= preSpaceWidth)
                                    tempLineWidthComp2 = preSpaceWidth;
                                else
                                    tempLineWidthComp2 = maxLineWidth;
                                maxLineWidth = tempLineWidthComp2;
                                lastWrapPosition = 0;
                                ++currentLineCount;
                                currentLineWidth -= postSpaceWidth;
                            }
                        }
                        else
                        {
                            if (processedTextLen + 3 >= textBufferSize)
                            {
                                dynamicTextBuffer = static_cast<char*>(ReallocateMem(reinterpret_cast<void*>(0x11F6238), dynamicTextBuffer, processedTextLen + 6));
                                textBufferSize = processedTextLen + 6;
                            }
                            dynamicTextBuffer[processedTextLen + 2] = dynamicTextBuffer[processedTextLen];
                            dynamicTextBuffer[processedTextLen + 1] = dynamicTextBuffer[processedTextLen - 1];
                            memcpy(&dynamicTextBuffer[processedTextLen - 1], "-\n", 2);
                            processedTextLen += 2;
                            hyphenInsertCount += 2;
                            pCurrentGlyph = &this->fontData->glyphs[45];
                            totalTextHeight = this->fontData->lineHeight + lineSpacingAdjust + totalTextHeight;
                            hyphenInsertWidth = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                            currentLineWidth -= hyphenInsertWidth;
                            AppendToListTail(&textParams->lineWidths.m_listHead.data, &currentLineWidth);
                            if (maxLineWidth <= currentLineWidth)
                                tempLineWidthComp1 = currentLineWidth;
                            else
                                tempLineWidthComp1 = maxLineWidth;
                            maxLineWidth = tempLineWidthComp1;
                            lastWrapPosition = 0;
                            ++currentLineCount;
                            pCurrentGlyph = &this->fontData->glyphs[dynamicTextBuffer[processedTextLen - 1]];
                            currentLineWidth = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                            pCurrentGlyph = &this->fontData->glyphs[currentChar];
                            combinedCharWidth = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                            currentLineWidth += combinedCharWidth;
                        }
                    }
                    if (currentChar != '~')
                        dynamicTextBuffer[processedTextLen++] = currentChar;
                    if (processedTextLen >= textBufferSize)
                    {
                        dynamicTextBuffer = static_cast<char*>(ReallocateMem(reinterpret_cast<void*>(0x11F6238), dynamicTextBuffer, processedTextLen + 4));
                        textBufferSize = processedTextLen + 4;
                    }
                }
                if (maxAllowedLines > 0 && currentLineCount > maxAllowedLines && processedTextLen)
                {
                    while (dynamicTextBuffer[processedTextLen] != textParams->newLineCharacter)
                        --processedTextLen;
                    dynamicTextBuffer[processedTextLen] = 0;
                    totalTextHeight = totalTextHeight - (this->fontData->lineHeight + lineSpacingAdjust);
                    break;
                }
            }
            if (*dynamicTextBuffer && textParams->initdToZero)
            {
                truncatedTextLen = 0;
                lineCounter = 0;
                for (truncateCharCounter = 0; truncateCharCounter < processedTextLen; ++truncateCharCounter)
                {
                    if (lineCounter >= textParams->initdToZero && lineCounter < textParams->wrapLines)
                        dynamicTextBuffer[truncatedTextLen++] = dynamicTextBuffer[truncateCharCounter];
                    if (dynamicTextBuffer[truncateCharCounter] == 10)
                        ++lineCounter;
                }
                dynamicTextBuffer[truncatedTextLen] = 0;
                processedTextLen = truncatedTextLen;
            }
            if (!*dynamicTextBuffer)
            {
                strcpy(dynamicTextBuffer, " ");
                processedTextLen = 1;
                currentLineCount = 1;
                totalTextHeight = this->fontData->glyphs[32].height;
                currentLineWidth = ConditionalFloatToUInt(this->fontData->glyphs[32].width);
            }
            AppendToListTail(&textParams->lineWidths.m_listHead.data, &currentLineWidth);
            if (maxLineWidth <= currentLineWidth)
                finalMaxLineWidth = currentLineWidth;
            else
                finalMaxLineWidth = maxLineWidth;
            maxLineWidth = finalMaxLineWidth;
            dynamicTextBuffer[processedTextLen] = 0;
            StringBuffer_Manage(textParams, dynamicTextBuffer, 0);
            textParams->wrapWidth = maxLineWidth;
            textParams->wrapLimit = totalTextHeight;
            textParams->initdToZero = 0;
            textParams->wrapLines = currentLineCount;
            textParams->length = processedTextLen;
            DeallocMem(reinterpret_cast<void*>(0x11F6238), processedOriginalText);
            DeallocMem(reinterpret_cast<void*>(0x11F6238), dynamicTextBuffer);
        }
    };

	class FontManagerEx : public FontManager {
	public:
        inline float GetHanziWidth(uint16_t gbChar, FontInfo::GlyphInfo* fontCharMetrics) {
			// Check if the character is a valid GB2312 character
            if (fontCharMetrics[gbChar].width > 0) {
                return fontCharMetrics[gbChar].kerningLeft
                    + fontCharMetrics[gbChar].width
                    + fontCharMetrics[gbChar].kerningRight;
            }

			// default space character width if the character is not found
            return (fontCharMetrics[' '].kerningLeft
                + fontCharMetrics[' '].width
                + fontCharMetrics[' '].kerningRight);
        }

        //	outDims.x := width (pxl); outDims.y := height (pxl); outDims.z := numLines
		NiVector3* __thiscall GetStringDimensionsEx(NiVector3* outDimensions, const char* srcString, uint32_t fontID, float maxWrapWidth, uint32_t startCharIndex) {
            double tabStopWidth; // st7
            float finalMaxLineWidth; // [esp+10h] [ebp-4Ch]
            float adjustedWrapWidth; // [esp+14h] [ebp-48h]
            float previousLineWidthAtTabStop; // [esp+18h] [ebp-44h]
            unsigned __int8 currentChar; // [esp+27h] [ebp-35h] BYREF
            float currentCharTotalWidth; // [esp+28h] [ebp-34h]
            signed int currentCharIndex; // [esp+2Ch] [ebp-30h]
            float previousCharTotalWidth; // [esp+30h] [ebp-2Ch]
            int totalLines; // [esp+34h] [ebp-28h]
            char hasHyphenationPoint; // [esp+3Bh] [ebp-21h]
            NiVector3 StringDimensions; // [esp+3Ch] [ebp-20h]
            float lastValidWrapPosition; // [esp+48h] [ebp-14h]
            float currentLineWidth; // [esp+4Ch] [ebp-10h]
            int sourceStringLength; // [esp+50h] [ebp-Ch]
            FontInfo::GlyphInfo* fontCharMetrics; // [esp+54h] [ebp-8h]
            float fontVerticalSpacingAdjust; // [esp+58h] [ebp-4h]

            if (fontID >= 1 && fontID <= 8 && srcString)
            {
                StringDimensions = StringDefaulDimensions;
                sourceStringLength = strlen(srcString);
                fontCharMetrics = this->fontInfos[fontID - 1]->fontData->glyphs;
                lastValidWrapPosition = 0.0;
                currentLineWidth = 0.0;
                fontVerticalSpacingAdjust = VertSpacingAdjust(fontID);
                previousCharTotalWidth = 0.0;
                hasHyphenationPoint = 0;
                totalLines = 1;
                StringDimensions.y = fontCharMetrics[' '].height;
                for (currentCharIndex = startCharIndex; currentCharIndex < sourceStringLength; ++currentCharIndex)
                {
                    currentChar = srcString[currentCharIndex];
                    currentCharTotalWidth = 0.0;
                    ConvertToAsciiQuotes(&currentChar);
                    currentCharTotalWidth = fontCharMetrics[currentChar].kerningLeft
                        + fontCharMetrics[currentChar].width
                        + fontCharMetrics[currentChar].kerningRight;
                    switch (currentChar)
                    {
                    case '\t':
                        tabStopWidth = currentLineWidth;
                        AlignLineWidthToTab(currentLineWidth, 75.0);
                        previousLineWidthAtTabStop = tabStopWidth;
                        currentCharTotalWidth = 75.0 - previousLineWidthAtTabStop;
                        break;
                    case '\n':
                    case ' ':
                        lastValidWrapPosition = currentLineWidth;
                        hasHyphenationPoint = 0;
                        break;
                    case '~':
                        lastValidWrapPosition = currentLineWidth
                            + fontCharMetrics['-'].kerningLeft
                            + fontCharMetrics['-'].width
                            + fontCharMetrics['-'].kerningRight;
                        hasHyphenationPoint = 1;
                        break;
                    default:
                        break;
                    }
                    if (currentChar != '~')
                        currentLineWidth = currentLineWidth + currentCharTotalWidth;
                    if (maxWrapWidth < currentLineWidth || currentChar == '\n')
                    {
                        if (lastValidWrapPosition <= 0.0)
                        {
                            lastValidWrapPosition = currentLineWidth
                                - currentCharTotalWidth
                                - previousCharTotalWidth
                                + fontCharMetrics['-'].kerningLeft
                                + fontCharMetrics['-'].width
                                + fontCharMetrics['-'].kerningRight;
                            currentLineWidth = currentCharTotalWidth + previousCharTotalWidth;
                        }
                        else
                        {
                            currentLineWidth = currentLineWidth - lastValidWrapPosition;
                            if (!hasHyphenationPoint)
                            {
                                if (currentChar == '\n')
                                    currentLineWidth = 0.0;
                                else
                                    currentLineWidth = currentLineWidth
                                    - (fontCharMetrics[' '].kerningLeft
                                        + fontCharMetrics[' '].width
                                        + fontCharMetrics[' '].kerningRight);
                            }
                        }
                        if (lastValidWrapPosition >= StringDimensions.x)
                            adjustedWrapWidth = lastValidWrapPosition;
                        else
                            adjustedWrapWidth = StringDimensions.x;
                        StringDimensions.x = adjustedWrapWidth;
                        StringDimensions.y = fontVerticalSpacingAdjust
                            + this->fontInfos[fontID - 1]->fontData->lineHeight
                            + StringDimensions.y;
                        lastValidWrapPosition = 0.0;
                        ++totalLines;
                    }
                    previousCharTotalWidth = currentCharTotalWidth;
                }
                if (currentLineWidth >= StringDimensions.x)
                    finalMaxLineWidth = currentLineWidth;
                else
                    finalMaxLineWidth = StringDimensions.x;
                StringDimensions.z = totalLines;
                outDimensions->x = finalMaxLineWidth;
                outDimensions->y = StringDimensions.y;
                outDimensions->z = StringDimensions.z;
                return outDimensions;
            }
            else
            {
                *outDimensions = StringDefaulDimensions;
                return outDimensions;
            }
		}
	};

	void InitVertSpacingHook() {
        WriteRelJump(0xA1B3A0, &VertSpacingAdjust);
	}

    void InitFontHook() {
        WriteRelJumpEx(0xA1B020, &FontManagerEx::GetStringDimensionsEx);
        WriteRelJumpEx(0xA12FB0, &FontInfoEx::ProcessTextLayoutEx);
    }
}