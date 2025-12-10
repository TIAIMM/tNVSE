#pragma once
#include <cstring>
#include <cstdint>
#include <limits>
#include "BSFile.hpp"
#include "MemoryManager.hpp"
#include "uidecode.h"
#include "fonthook.h"

namespace fonthook {
    void __cdecl ConvertToAsciiQuotes(UInt8* currentChar) {
        CdeclCall(0xA122B0, currentChar);
    }

    bool __cdecl ReplaceVariableInString(const char* varName, char* outBuffer, UInt32 bufferSize, bool isPositiveEscape) {
        return CdeclCall<bool>(0x7070C0, varName, outBuffer, bufferSize, isPositiveEscape);
    }

    bool __cdecl ParseAndFormatVariableInString(const char* p_varNameBuffer, void* p_parsedTextBuffer) {
        return CdeclCall<bool>(0x7073D0, p_varNameBuffer, p_parsedTextBuffer);
    }

    SInt32 __cdecl AlignLineWidthToTab(double a1, double a2) {
        return CdeclCall<SInt32>(0xEC9130, a1, a2);
    }

    BSFile* __cdecl LoadFile(const char* filePath, SInt32 loadMode, UInt32 allocFlags, SInt32 openMode) {
        return CdeclCall<BSFile*>(0xAFDF20, filePath, loadMode, allocFlags, openMode);
    }

    BSFile* __cdecl FileFinder_GetFile(const char* apName, SInt32 aeMode, UInt32 aiSize, SInt32 aiArchiveType) {
        return CdeclCall<BSFile*>(0xAFDF20, apName, aeMode, aiSize, aiArchiveType);
    }

    void* __cdecl AppendToListTail(void* ListNode, void * ListNode2) {
        return ThisStdCall<void*>(0xAF25B0, ListNode, ListNode2);
    }

    // 0xEC62F6
    uint32_t SafeDoubleToUInt32(double a1)
    {
        uint64_t bits;
        std::memcpy(&bits, &a1, sizeof(bits));

        uint32_t low = static_cast<uint32_t>(bits);
        uint32_t high = static_cast<uint32_t>(bits >> 32);

        if (low == 0 && (high & 0x7FFFFFFF) == 0)
            return 0;

        bool sign = (high & 0x80000000u) != 0;

        if (!sign)
        {
            return low;
        }
        else
        {
            uint64_t pair64 = (static_cast<uint64_t>(0) << 32) | low;
            return static_cast<uint32_t>((pair64 + 0x7FFFFFFF) >> 32);
        }
    }

    // 0xEC62C0
    uint32_t ConditionalFloatToUInt(double a1)
    {
        if (*(volatile uint32_t*)0x01270A6C)
            return static_cast<uint32_t>(a1);
        else
            return SafeDoubleToUInt32(a1);
    }

    Float32 __stdcall GetFontVerticalSpacingAdjustment(UInt32 fontID) {
        return StdCall<Float32>(0xA1B3A0, fontID);
    }

    Float32 __stdcall VertSpacingAdjust(UInt32 fontID) {
        return 0;
    }

    bool IsGB2312LeadByte(unsigned char c) {
        return (c >= 0xA1 && c <= 0xF7);
    }

    typedef uint32_t(__thiscall* GetFileSizeFunc)(void* pThis);

    uint32_t GetFileSize(void* fntFileHandle) {
        void** vtable = *(void***)fntFileHandle;
        GetFileSizeFunc func = (GetFileSizeFunc)vtable[10];
        return func(fntFileHandle);
    }

    NiVector3& StringDefaulDimensions = *reinterpret_cast<NiVector3*>(0x11F426C);

    MemoryManager* MemoryManagerSingleton = reinterpret_cast<MemoryManager*>(0x11F6238);
    
    MemoryManager* MemoryManager_s_Instance = reinterpret_cast<MemoryManager*>(0x11F6238);

    class FontInfoEx : public FontInfo {
    public:
        UINT32 __thiscall GenerateTextGeometryEx(
            BSString* srcText,
            UINT32* unk01,
            UINT32* unk02,
            int initdToZero,
            int initdToMaxInt,
            UINT32 alignmentType,                      // (2=Center, 4=Right Align)
            char lineBreakChar,
            void* unk04,
            void** unk05,
            void** unk06) {

            //const char* str = srcText->pString;
            //gLog.FormattedMessage("str = '%s'", str);
            //gLog.FormattedMessage("WrapWidth = %u", *unk01);
            //gLog.FormattedMessage("WrapLimit = %u", *unk02);

            UINT32 ret = ThisStdCall<UInt32>(
                0xA12880,
                this,
                srcText,
                unk01,
                unk02,
                initdToZero,
                initdToMaxInt,
                alignmentType,
                lineBreakChar,
                unk04,
                unk05,
                unk06);

            return ret;
        }

        void __thiscall CalculateTextLayoutEx(const char* textSrc, FontTextReplaced* textParams) {
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
            UInt32 charIndex; // [esp+B0h] [ebp-730h]
            UInt32 bufferCopyIndex; // [esp+B4h] [ebp-72Ch]
            char textureNameBuffer[268]; // [esp+B8h] [ebp-728h] BYREF
            int charScanIndex; // [esp+1C4h] [ebp-61Ch]
            float unkarray[4]; // [esp+1D8h] [ebp-608h]
            char substrBuffer[264]; // [esp+1E8h] [ebp-5F8h] BYREF
            signed int escapeSeqSizeDiff; // [esp+2F0h] [ebp-4F0h]
            UInt32 postEscapeTextLen; // [esp+2F4h] [ebp-4ECh]
            bool isPositiveEscape; // [esp+2FBh] [ebp-4E5h]
            int escapeSeqPrefixLen; // [esp+2FCh] [ebp-4E4h]
            UInt32 totalEscapeSeqLen; // [esp+300h] [ebp-4E0h]
            int varNameLen; // [esp+304h] [ebp-4DCh]
            char varNameBuffer[33]; // [esp+308h] [ebp-4D8h] BYREF
            unsigned int srcTextIndex; // [esp+38Ch] [ebp-454h]
            UInt32 currentLineWidth; // [esp+390h] [ebp-450h] BYREF
            unsigned int processedTextLen; // [esp+394h] [ebp-44Ch]
            unsigned int lastWrapPosition; // [esp+398h] [ebp-448h]
            UInt32 maxLineWidth; // [esp+39Ch] [ebp-444h]
            char* dynamicTextBuffer; // [esp+3A0h] [ebp-440h]
            UInt32 buttonIconIndex; // [esp+3A4h] [ebp-43Ch]
            UInt32 preSpaceWidth; // [esp+3A8h] [ebp-438h] BYREF
            bool hasEscapeSequence; // [esp+3AFh] [ebp-431h]
            signed int postSpaceWidth; // [esp+3B0h] [ebp-430h]
            bool isTildeChar; // [esp+3B7h] [ebp-429h]
            char* processedOriginalText; // [esp+3B8h] [ebp-428h]
            int currentLineCount; // [esp+3BCh] [ebp-424h]
            UInt32 textBufferSize; // [esp+3C0h] [ebp-420h]
            int hyphenInsertCount; // [esp+3C4h] [ebp-41Ch]
            char parsedTextBuffer[1028]; // [esp+3C8h] [ebp-418h] BYREF
            float lineSpacingAdjust; // [esp+7D0h] [ebp-10h]
            float totalTextHeight; // [esp+7D4h] [ebp-Ch]
            UInt32 sourceTextLen; // [esp+7D8h] [ebp-8h]
            int maxAllowedLines; // [esp+7DCh] [ebp-4h]

            //gLog.Message("Call CalculateTextLayoutEx");

            if (!textSrc)
                return;

            //gLog.FormattedMessage("textSrc = '%s'", textSrc);

            if (textParams->wrapWidth <= 0)
                textParams->wrapWidth = 0x7FFFFFFF;
            if (textParams->wrapLimit <= 0)
                textParams->wrapLimit = 0x7FFFFFFF;
            if (textParams->wrapLines <= 0)
                textParams->wrapLines = 0x7FFFFFFF;

            lineSpacingAdjust = GetFontVerticalSpacingAdjustment(this->fontID);
            lastWrapPosition = 0;
            preSpaceWidth = 0;
            postSpaceWidth = 0;
            currentLineWidth = 0;
            maxLineWidth = 0;
            totalTextHeight = this->fontData->glyphs[' '].height;
            currentLineCount = 1;
            // 0xEC6130
            sourceTextLen = strlen(textSrc);
            //gLog.FormattedMessage("sourceTextLen = %u", sourceTextLen);
            maxAllowedLines = textParams->wrapLines;

            //gLog.Message("Init originalTextBuffer");
            //gLog.FormattedMessage("Allocating originalTextBuffer: size=%d", sourceTextLen + 4);
            originalTextBuffer = static_cast<char*>(MemoryManagerSingleton->Allocate(sourceTextLen + 4));
            if (!originalTextBuffer) {
                //gLog.Message("Memory allocation failed for originalTextBuffer");
                return;
            }

            //gLog.Message("MemSet originalTextBuffer");
            // 0xEC61C0
            memset(originalTextBuffer, 0, sourceTextLen + 4);

            //gLog.Message("Init processedOriginalText");
            processedOriginalText = originalTextBuffer;


            //gLog.Message("Init processedTextBuffer");
            //gLog.FormattedMessage("Allocating processedTextBuffer: size=%u", sourceTextLen + 4);
            processedTextBuffer = static_cast<char*>(MemoryManagerSingleton->Allocate(sourceTextLen + 4));
            if (!processedTextBuffer) {
                //gLog.Message("Memory allocation failed for processedTextBuffer");
                MemoryManagerSingleton->Deallocate(originalTextBuffer);
                return;
            }

            //gLog.Message("MemSet processedTextBuffer");
            // 0xEC61C0
            memset(processedTextBuffer, 0, sourceTextLen + 4);

            //gLog.Message("Init dynamicTextBuffer");
            dynamicTextBuffer = processedTextBuffer;

            //gLog.Message("SafeFormatString originalTextBuffer");
            // 0xEC623A
            snprintf(originalTextBuffer, sourceTextLen + 1, "%s", textSrc);
            //gLog.Message("Buffer Init Finish");

            processedTextLen = 0;
            textBufferSize = sourceTextLen + 4;
            hyphenInsertCount = 0;
            isTildeChar = 0;
            parsedTextBuffer[0] = 0;
            hasEscapeSequence = 0;
            //gLog.Message("processedOriginalText Read Start");
            for (srcTextIndex = 0; srcTextIndex < sourceTextLen; ++srcTextIndex)
            {
                //gLog.FormattedMessage("process index %u", srcTextIndex);
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
                    // 0xEC6130
                    totalEscapeSeqLen = (strlen(varNameBuffer) + 1);
                    if (processedOriginalText[varNameLen + srcTextIndex] == ';')
                        totalEscapeSeqLen += escapeSeqPrefixLen;
                    //gLog.Message("ReplaceVariableInString & ParseAndFormatVariableInString");
                    if (ReplaceVariableInString(varNameBuffer, parsedTextBuffer, 0x400u, isPositiveEscape)
                        || ParseAndFormatVariableInString(varNameBuffer, parsedTextBuffer))
                    {
                        // 0xEC6130
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
                            // 0xEC6370
                            //gLog.Message("FastStringCopyAligned 1");
                            //strcpy(&parsedTextBuffer[charScanIndex + 1], substrBuffer);
                            strcpy_s(&parsedTextBuffer[charScanIndex + 1],
                                sizeof(parsedTextBuffer) - (charScanIndex + 1),
                                substrBuffer);
                            //gLog.Message("FastStringCopyAligned 1 End");
                            // 0xEC6130
                            UInt32 strLen = strlen(substrBuffer);
                            *((char*)unkarray + strLen + 15) = 0;
                            if (this->fontID == 7)
                            {
                                // 0xEC65A6
                                strncpy_s(textureNameBuffer, sizeof(textureNameBuffer), substrBuffer, _TRUNCATE);
                                // 0x406D00
                                vsnprintf(substrBuffer, sizeof(substrBuffer), "glow_%s", textureNameBuffer);
                            }
                            this->LoadFontIcon(substrBuffer);
                            parsedTextBuffer[charScanIndex] = 1;
                            parsedTextBuffer[charScanIndex + 1] = 0;
                            // 0xEC6130
                            postEscapeTextLen = strlen(parsedTextBuffer);
                            escapeSeqSizeDiff = postEscapeTextLen - totalEscapeSeqLen;
                        }
                        if (escapeSeqSizeDiff > 0)
                        {
                            textBufferSize += escapeSeqSizeDiff;
                            dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, textBufferSize));
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
                    //gLog.FormattedMessage("dynamicTextBuffer [%u] = processedOriginalText[%u]", processedTextLen + 1 , srcTextIndex);
                    dynamicTextBuffer[processedTextLen++] = processedOriginalText[srcTextIndex];
                    //gLog.Message("Copy Finished");
                }
            }
            //gLog.Message("processedOriginalText Read Finish");
            dynamicTextBuffer[processedTextLen] = 0;
            if (hasEscapeSequence)
            {
                sourceTextLen = processedTextLen;
                //gLog.FormattedMessage("Relocating processedOriginalText: size=%u", processedTextLen + 4);
                processedOriginalText = static_cast<char*>(MemoryManagerSingleton->Reallocate(processedOriginalText, processedTextLen + 4));
                // 0xEC6370
                //gLog.Message("FastStringCopyAligned");
                //strcpy(processedOriginalText, dynamicTextBuffer);
                strcpy_s(processedOriginalText, processedTextLen + 4, dynamicTextBuffer);
            }
            *dynamicTextBuffer = 0;
            processedTextLen = 0;
            buttonIconIndex = 0;

            //gLog.Message("processedOriginalText Read 2 Start");
            for (charIndex = 0; charIndex < sourceTextLen && processedOriginalText[charIndex]; ++charIndex)
            {
                if (processedOriginalText[charIndex] == textParams->newLineCharacter)
                {
                    dynamicTextBuffer[processedTextLen] = textParams->newLineCharacter;
                    if (++processedTextLen >= textBufferSize)
                    {
                        //gLog.FormattedMessage("Relocating dynamicTextBuffer: size=%u", processedTextLen + 4);
                        dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, processedTextLen + 4));
                        textBufferSize = processedTextLen + 4;
                    }
                    totalTextHeight = this->fontData->lineHeight + lineSpacingAdjust + totalTextHeight;
                    //gLog.FormattedMessage("AppendToListTail: %d", currentLineWidth);
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
                    //gLog.FormattedMessage("ConvertToAsciiQuotes: '%c'", currentChar);
                    ConvertToAsciiQuotes(&currentChar);
                    //gLog.FormattedMessage("ConvertToAsciiQuotes: '%c' end", currentChar);
                    pCurrentGlyph = &this->fontData->glyphs[currentChar];
                    if (currentChar == 1)
                    {
                        if (buttonIconIndex < this->buttonIconArray.uiSize)
                        {
                            pCurrentGlyph->width = this->buttonIconArray.pBuffer[buttonIconIndex].unk01;
                            pCurrentGlyph->kerningRight = this->buttonIconArray.pBuffer[buttonIconIndex].unk04;
                        }
                        ++buttonIconIndex;
                    }
                    //gLog.FormattedMessage("ConditionalFloatToUInt: %d", pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                    charWidthWithKerning = ConditionalFloatToUInt(pCurrentGlyph->width + pCurrentGlyph->kerningRight);
                    //gLog.FormattedMessage("charWidthWithKerning: %d", charWidthWithKerning);
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
                    //gLog.FormattedMessage("currentChar '%c' process end", currentChar);
                    if (currentLineWidth > textParams->wrapWidth)
                    {
                        if (lastWrapPosition)
                        {
                            if (isTildeChar)
                            {
                                isTildeChar = 0;
                                textBufferSize += 4;
                                dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, textBufferSize));
                                // 0xEC7230
                                memmove(
                                    &dynamicTextBuffer[lastWrapPosition + 2],
                                    &dynamicTextBuffer[lastWrapPosition],
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
                                dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, processedTextLen + 6));
                                textBufferSize = processedTextLen + 6;
                            }
                            dynamicTextBuffer[processedTextLen + 2] = dynamicTextBuffer[processedTextLen];
                            dynamicTextBuffer[processedTextLen + 1] = dynamicTextBuffer[processedTextLen - 1];
                            //qmemcpy
                            memcpy(&dynamicTextBuffer[processedTextLen - 1], "-\n", 2);
                            processedTextLen += 2;
                            hyphenInsertCount += 2;
                            pCurrentGlyph = &this->fontData->glyphs['-'];
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
                        dynamicTextBuffer = static_cast<char*>(MemoryManagerSingleton->Reallocate(dynamicTextBuffer, processedTextLen + 4));
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
            //gLog.Message("processedOriginalText Read 2 End");
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
                //strcpy
                strcpy_s(dynamicTextBuffer, textBufferSize, " ");
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
            textParams->str.Set(dynamicTextBuffer, 0);
            textParams->wrapWidth = maxLineWidth;
            textParams->wrapLimit = totalTextHeight;
            textParams->initdToZero = 0;
            textParams->wrapLines = currentLineCount;
            textParams->length = processedTextLen;
            MemoryManagerSingleton->Deallocate(processedOriginalText);
            MemoryManagerSingleton->Deallocate(dynamicTextBuffer);
            //gLog.Message("CalculateTextLayoutEx End");
        }

        void __thiscall LoadFontDataEx() {
            NiTexturingProperty* resourceHandleTemp2; // [esp+10h] [ebp-23Ch]
            NiPixelData* texturingPropertyTemp2; // [esp+18h] [ebp-234h]
            float newMaxWidthMod; // [esp+20h] [ebp-22Ch]
            float newMaxGlyphHeight; // [esp+24h] [ebp-228h]
            float newMaxHeight; // [esp+28h] [ebp-224h]
            const char* fontFilePathTemp; // [esp+58h] [ebp-1F4h]
            void* textureDataSize; // [esp+60h] [ebp-1ECh]
            UInt32 bytesRead3; // [esp+64h] [ebp-1E8h]
            UInt32 readFlag; // [esp+6Ch] [ebp-1E0h] BYREF
            UInt32 bytesRead1Temp; // [esp+70h] [ebp-1DCh]
            UInt32 bytesRead1; // [esp+74h] [ebp-1D8h]
            UInt32 readParams1[2]; // [esp+78h] [ebp-1D4h] BYREF
            bool isTexValid; // [esp+83h] [ebp-1C9h]
            UInt32 savedTlsValueTemp; // [esp+84h] [ebp-1C8h]
            //const char* fontFilePath; // [esp+88h] [ebp-1C4h]
            float currentMaxWidthMod; // [esp+8Ch] [ebp-1C0h]
            float currentWidthMod; // [esp+90h] [ebp-1BCh]
            float currentGlyphHeight; // [esp+94h] [ebp-1B8h]
            float currentMaxHeight; // [esp+98h] [ebp-1B4h]
            UInt32 fileSize; // [esp+9Ch] [ebp-1B0h]
            FontInfo::BufferData* fontBuffer; // [esp+A0h] [ebp-1ACh]
            UInt32 bytesRead2Temp; // [esp+A4h] [ebp-1A8h]
            UInt32 bytesRead2; // [esp+A8h] [ebp-1A4h]
            UInt32 readParams2[2]; // [esp+ACh] [ebp-1A0h] BYREF
            //const char* fontFilePathError; // [esp+B4h] [ebp-198h]
            __int16 isLoadedFlag; // [esp+BAh] [ebp-192h]
            UInt32 oldTlsValue; // [esp+BCh] [ebp-190h]
            int stringRefFlag; // [esp+C0h] [ebp-18Ch]
            BSFile* texFileHandleTemp; // [esp+C4h] [ebp-188h]
            BSFile* texFileVTable; // [esp+C8h] [ebp-184h]
            const char* fontPathCopy; // [esp+CCh] [ebp-180h] BYREF
            NiTexturingProperty* resourceTemp; // [esp+D0h] [ebp-17Ch]
            NiTexturingProperty* resourceHandleTemp; // [esp+D4h] [ebp-178h]
            NiPixelData* fontTextureObject; // [esp+D8h] [ebp-174h]
            NiPixelData* fontTexturingPropertyTemp; // [esp+DCh] [ebp-170h]
            BSFile* texFileHandleTemp2; // [esp+E0h] [ebp-16Ch]
            BSFile* texFileHandleVTable; // [esp+E4h] [ebp-168h]
            void* File_1; // [esp+E8h] [ebp-164h]
            BSFile* FileVTable2; // [esp+ECh] [ebp-160h]
            FontInfo::BufferData* bufferData; // [esp+F0h] [ebp-15Ch]
            BSFile* File_3; // [esp+F4h] [ebp-158h]
            BSFile* FileVTable1; // [esp+F8h] [ebp-154h]
            UInt32 texWidth; // [esp+FCh] [ebp-150h] BYREF
            UInt32 texHeight; // [esp+100h] [ebp-14Ch]
            UInt32 textureCreateArgs[3]; // [esp+104h] [ebp-148h] BYREF
            BSFile* texFileHandle; // [esp+110h] [ebp-13Ch]
            NiTexturingProperty* resourceHandle; // [esp+114h] [ebp-138h]
            NiPixelData* texturingProperty; // [esp+118h] [ebp-134h]
            UInt32 texIndex; // [esp+11Ch] [ebp-130h]
            float glyphTotalHeight; // [esp+120h] [ebp-12Ch]
            UInt32 glyphIndex; // [esp+124h] [ebp-128h]
            char fontTexPath[260]; // [esp+128h] [ebp-124h] BYREF
            float tempWidth; // [esp+230h] [ebp-1Ch]
            BSFile* fntFileHandle; // [esp+234h] [ebp-18h]
            float maxGlyphHeight; // [esp+238h] [ebp-14h]
            UInt32 savedTlsValue; // [esp+23Ch] [ebp-10h]
            int stackCookie; // [esp+248h] [ebp-4h]

            DWORD tebAddress;
            DWORD tlsPointer;
            DWORD tlsSlotAddress;
            DWORD targetAddress;
            DWORD* pTlsIndex = (DWORD*)0x126FD98;

            gLog.Message("Call LoadFontDataEx");

            __asm {
                mov eax, fs: [0x18]
                mov tebAddress, eax
            }

            gLog.Message("Get tebAddress");

            tlsPointer = *(DWORD*)(tebAddress + 0x2C);

            tlsSlotAddress = *(DWORD*)(tlsPointer + (*pTlsIndex) * 4);
            targetAddress = tlsSlotAddress + 692;

            stringRefFlag = 0;

            oldTlsValue = *(DWORD*)targetAddress;
            savedTlsValue = oldTlsValue;
            *(DWORD*)targetAddress = 12;

            gLog.Message("TLS Init");

            stackCookie = 0;
            isLoadedFlag = *&this->isLoaded;
            if (isLoadedFlag || !this->filePath)
            {
            LABEL_46:
                gLog.Message("LABEL_46");
                ++*&this->isLoaded;
                stackCookie = -1;
                *(DWORD*)targetAddress = savedTlsValue;
                gLog.Message("Call LoadFontData End\n");
                return;
            }
            gLog.Message("Load File 1");
            fntFileHandle = LoadFile(this->filePath, 0, 0x4000u, 2);
            gLog.Message("Load File 1 end");
            if (fntFileHandle)
            {
                if (fntFileHandle->m_bGood)
                {
                    gLog.Message("fnt file valid");
                    gLog.Message("Allocate font buffer data");
                    bufferData = static_cast<FontInfo::BufferData*>(MemoryManagerSingleton->Allocate(0x3928u));
                    this->fontData = bufferData;
                    gLog.Message("operator bool");
                    // fileSize = fntFileHandle->Unk_0A(fntFileHandle);
                    //fileSize = GetFileSize(fntFileHandle);
                    fileSize = fntFileHandle->operator bool();
                    gLog.Message("operator bool finish");
                    fontBuffer = this->fontData;
                    readParams2[0] = 1;
                    bytesRead2Temp = fntFileHandle->m_pfnRead(fntFileHandle, fontBuffer, fileSize, readParams2, 1u);
                    fntFileHandle->m_uiAbsoluteCurrentPos += bytesRead2Temp;
                    bytesRead2 = bytesRead2Temp;
                    File_1 = fntFileHandle;
                    FileVTable2 = fntFileHandle;
                    if (fntFileHandle)
                        FileVTable2->~BSFile();
                    fntFileHandle = 0;
                    this->maxCharHeight = 0.0;
                    maxGlyphHeight = 0.0;
                    this->maxWidthMod = 0.0;
                    for (glyphIndex = 0; glyphIndex < 256; ++glyphIndex)
                    {
                        glyphTotalHeight = this->fontData->lineHeight - this->fontData->glyphs[glyphIndex].ascent;
                        glyphTotalHeight = glyphTotalHeight + this->fontData->glyphs[glyphIndex].height;
                        currentMaxHeight = this->maxCharHeight;
                        if (glyphTotalHeight >= currentMaxHeight)
                            newMaxHeight = glyphTotalHeight;
                        else
                            newMaxHeight = currentMaxHeight;
                        this->maxCharHeight = newMaxHeight;
                        currentGlyphHeight = this->fontData->glyphs[glyphIndex].height;
                        if (currentGlyphHeight >= maxGlyphHeight)
                            newMaxGlyphHeight = currentGlyphHeight;
                        else
                            newMaxGlyphHeight = maxGlyphHeight;
                        maxGlyphHeight = newMaxGlyphHeight;
                        currentMaxWidthMod = this->maxWidthMod;
                        currentWidthMod = this->fontData->glyphs[glyphIndex].ascent - this->fontData->glyphs[glyphIndex].height;
                        if (currentWidthMod <= currentMaxWidthMod)
                            newMaxWidthMod = currentWidthMod;
                        else
                            newMaxWidthMod = currentMaxWidthMod;
                        this->maxWidthMod = newMaxWidthMod;
                    }
                    gLog.Message("Fnt Head Read Finished");
                    tempWidth = this->fontData->glyphs[' '].width;
                    this->fontData->glyphs[' '].width = this->fontData->glyphs[' '].kerningRight;
                    this->fontData->glyphs[' '].kerningRight = tempWidth;
                    this->fontData->glyphs[' '].height = maxGlyphHeight;
                    this->fontData->glyphs[' '].ascent = this->maxWidthMod + maxGlyphHeight;
                    this->fontData->glyphs[160].width = this->fontData->glyphs[' '].width;
                    this->fontData->glyphs[160].kerningRight = this->fontData->glyphs[' '].kerningRight;
                    this->fontData->glyphs[160].height = this->fontData->glyphs[' '].height;
                    this->fontData->glyphs[160].ascent = this->fontData->glyphs[' '].ascent;
                    this->fontData->glyphs[127].width = this->fontData->glyphs['|'].width;
                    this->fontData->glyphs[127].kerningLeft = this->fontData->glyphs['|'].kerningLeft;
                    this->fontData->glyphs[127].kerningRight = this->fontData->glyphs['|'].kerningRight;
                    this->fontData->glyphs[127].height = this->fontData->glyphs['|'].height;
                    this->fontData->glyphs[127].ascent = this->fontData->glyphs['|'].ascent;
                    this->fontData->glyphs['\0'].width = 0.0;
                    this->fontData->glyphs['\0'].kerningRight = 0.0;
                    this->fontData->glyphs['\0'].height = maxGlyphHeight;
                    this->fontData->glyphs['\0'].ascent = this->maxWidthMod + maxGlyphHeight;
                    this->fontData->glyphs['\0'].topLeft.x = 0.0;
                    this->fontData->glyphs['\0'].topRight.x = 0.0;
                    this->fontData->glyphs['\0'].bottomLeft.x = 0.0;
                    this->fontData->glyphs['\0'].bottomRight.x = 0.0;
                    this->fontData->glyphs['\0'].topLeft.y = 0.0;
                    this->fontData->glyphs['\0'].topRight.y = 0.0;
                    this->fontData->glyphs['\0'].bottomLeft.y = 0.0;
                    this->fontData->glyphs['\0'].bottomRight.y = 0.0;
                    gLog.Message("Special Char process end");
                    gLog.FormattedMessage("numTextures: %u", this->fontData->numTextures);
                    if (this->fontData->numTextures > 8)
                    {
                        //fontFilePath = this->filePath;
                        // 0x5B5E40 Return 0
                        stackCookie = -1;
                        savedTlsValueTemp = savedTlsValue;
                        *(DWORD*)targetAddress = savedTlsValue;
                        return;
                    }
                    gLog.Message("numTextures normal");
                    for (texIndex = 0; texIndex < this->fontData->numTextures; ++texIndex)
                    {
                        gLog.Message("Load Tex");
                        // 0x406D00
                        _snprintf_s(
                            fontTexPath,
                            0x100u,
                            _TRUNCATE,
                            "TEXTURES\\FONTS\\%s.TEX",
                            this->fontData->textures[texIndex].fileName
                        );

                        gLog.Message("LoadFile 2 start");
                        texFileHandle = LoadFile(fontTexPath, 0, 0x4000u, 2);
                        gLog.Message("LoadFile 2 end");

                        if (!texFileHandle || !(isTexValid = texFileHandle->m_bGood))
                        {
                            gLog.Message("Tex not valid");
                            // 0x5B5E40 Return 0
                            if (texFileHandle)
                            {
                                texFileHandleTemp2 = texFileHandle;
                                texFileHandleVTable = texFileHandle;
                                texFileHandleVTable->~BSFile();
                            }
                            stackCookie = -1;
                            readParams1[1] = savedTlsValue;
                            *(DWORD*)targetAddress = savedTlsValue;
                            return;
                        }
                        readParams1[0] = 1;

                        gLog.FormattedMessage("tex file valid");

                        bytesRead1Temp = texFileHandle->m_pfnRead(texFileHandle, &texWidth, 8u, readParams1, 1u);
                        texFileHandle->m_uiAbsoluteCurrentPos += bytesRead1Temp;

                        bytesRead1 = bytesRead1Temp;
                        textureCreateArgs[0] = 6;
                        textureCreateArgs[1] = 3;
                        textureCreateArgs[2] = 2;

                        fontTextureObject = CdeclCall<NiPixelData*>(0xAA13E0, 0x74);

                        gLog.FormattedMessage("fontTextureObject: %p", fontTextureObject);

                        stackCookie = (stackCookie & 0xFFFFFF00) | 1;
                        if (fontTextureObject) {
                            gLog.FormattedMessage("NiPixelData::InitializePixelData");
                            texturingPropertyTemp2 = ThisStdCall<NiPixelData*>(
                                0xA7C190,
                                fontTextureObject,
                                texWidth,
                                texHeight,
                                reinterpret_cast<const void*>(0x11AA2A0),
                                1,
                                1
                            );
                        }
                        else {
                            texturingPropertyTemp2 = 0;
                            stackCookie = -1;
                            savedTlsValueTemp = savedTlsValue;
                            *(DWORD*)targetAddress = savedTlsValue;
                            return;
                        }
                        fontTexturingPropertyTemp = texturingPropertyTemp2;
                        stackCookie = (stackCookie & 0xFFFFFF00) | 0;
                        texturingProperty = texturingPropertyTemp2;
                        textureDataSize = &texturingPropertyTemp2->m_pucPixels[*texturingPropertyTemp2->m_puiOffsetInBytes];
                        readFlag = 1;
                        bytesRead3 = texFileHandle->m_pfnRead(texFileHandle, textureDataSize, 4 * texHeight * texWidth, &readFlag, 1u);
                        texFileHandle->m_uiAbsoluteCurrentPos += bytesRead3;

                        texturingProperty->bNoConvert = 1;

                        resourceTemp = CdeclCall<NiTexturingProperty*>(0xAA13E0, 0x30);

                        gLog.FormattedMessage("resourceTemp: %p", resourceTemp);

                        stackCookie = (stackCookie & 0xFFFFFF00) | 2;
                        gLog.Message("Check resources");
                        if (resourceTemp)
                        {
                            gLog.FormattedMessage("Resource Init");
                            fontFilePathTemp = this->filePath;
                            if (fontFilePathTemp){
                                gLog.FormattedMessage("Cdeclcall A5B690 start");
                                fontPathCopy = CdeclCall<const char*>(0xA5B690, fontFilePathTemp);
                                gLog.FormattedMessage("Cdeclcall A5B690 end");
                            }
                            else
                                fontPathCopy = 0;
                            stackCookie = (stackCookie & 0xFFFFFF00) | 3;
                            stringRefFlag |= 1u;
                            gLog.FormattedMessage("texturingProperty: %p", texturingProperty);
                            gLog.FormattedMessage("fontPathCopy: %s", fontPathCopy);
                            gLog.FormattedMessage("textureCreateArgs: %p", textureCreateArgs);
                            gLog.FormattedMessage("CreateFontTexture start");
                            // NiTexturingProperty::CreateFontTexture
                            resourceHandleTemp2 = ThisStdCall<NiTexturingProperty*>(
                                0xA6ABB0,
                                resourceTemp,
                                texturingProperty,
                                &fontPathCopy,
                                textureCreateArgs);
                            gLog.FormattedMessage("CreateFontTexture end");
                        }
                        else
                        {
                            resourceHandleTemp2 = 0;
                        }

                        gLog.Message("Resources load end");
                        resourceHandleTemp = resourceHandleTemp2;
                        resourceHandle = resourceHandleTemp2;
                        stackCookie = 0;
                        if ((stringRefFlag & 1) != 0)
                        {
                            stringRefFlag &= ~1u;
                            if (fontPathCopy) {
                                char* mutablePtr = const_cast<char*>(fontPathCopy);
                                volatile LONG* refCount = reinterpret_cast<volatile LONG*>(mutablePtr - 2);
                                InterlockedDecrement(refCount);
                            }
                        }
                        texFileHandleTemp = texFileHandle;
                        texFileVTable = texFileHandle;
                        if (texFileHandle)
                            texFileVTable->~BSFile();
                        texFileHandle = 0;
                        if (resourceHandle) {
                            gLog.FormattedMessage("resourceHandle: %p", resourceHandle);
                        }
                        ThisStdCall(0x60AEB0, resourceHandle, 1);
                        ThisStdCall(0x66B0D0, &this->fontTexProp + texIndex, resourceHandle);
                        gLog.Message("finish load");
                    }
                    goto LABEL_46;
                }
            }
            //fontFilePathError = this->filePath;
            // 0x5B5E40 Return 0
            if (fntFileHandle)
            {
                File_3 = fntFileHandle;
                FileVTable1 = fntFileHandle;
                FileVTable1->~BSFile();
            }
            stackCookie = -1;
            readParams2[1] = savedTlsValue;
            *(DWORD*)targetAddress = savedTlsValue;
        }
    };

    class NiTexturingPropertyEx : public NiTexturingProperty {
    public:
        NiTexturingProperty* NiTexturingPropertyBuild(
            NiPixelData* pkPixelData,
            const NiFixedString* kName,
            NiTexture::FormatPrefs* arPrefs) {
            gLog.FormattedMessage("\nCall NiTexturingPropertyBuild");
            const char* sName = kName->m_kHandle ? kName->m_kHandle : "<null>";

            gLog.FormattedMessage("source: %p", this);
            gLog.FormattedMessage("pkPixelData: %p", pkPixelData);

            gLog.FormattedMessage("kName: %s", sName);
            gLog.FormattedMessage("m_ePixelLayout: %x", arPrefs->m_ePixelLayout);
            gLog.FormattedMessage("m_eAlphaFmt: %x", arPrefs->m_eAlphaFmt);
            gLog.FormattedMessage("m_eMipMapped: %x", arPrefs->m_eMipMapped);

            NiTexturingProperty* ret = ThisStdCall<NiTexturingProperty*>(
                0xA6ABB0,
                this,
                pkPixelData,
                kName,
                arPrefs);
            gLog.FormattedMessage("NiTexturingPropertyBuild End\n");
            return ret;
        }
    };

    NiTexturingProperty* NiTexturingPropertyBuild2(
        NiTexturingProperty* source,
        NiPixelData* pkPixelData,
        const NiFixedString* kName,
        NiTexture::FormatPrefs* arPrefs) {
        gLog.FormattedMessage("\nCall NiTexturingPropertyBuild");
        const char* sName = kName->m_kHandle ? kName->m_kHandle : "<null>";

        gLog.FormattedMessage("source: %p", source);
        gLog.FormattedMessage("pkPixelData: %p", pkPixelData);

        gLog.FormattedMessage("kName: %s", sName);
        gLog.FormattedMessage("m_ePixelLayout: %x", arPrefs->m_ePixelLayout);
        gLog.FormattedMessage("m_eAlphaFmt: %x", arPrefs->m_eAlphaFmt);
        gLog.FormattedMessage("m_eMipMapped: %x", arPrefs->m_eMipMapped);

        NiTexturingProperty* ret = ThisStdCall<NiTexturingProperty*>(
            0xA6ABB0,
            source,
            pkPixelData,
            kName,
            arPrefs);
        gLog.FormattedMessage("NiTexturingPropertyBuild End\n");
        return ret;
    }

    class NiPixelDataEx : public NiPixelData {
    public:
        NiPixelData* NiPixelDataBuild(
            unsigned int uiWidth,
            unsigned int uiHeight,
            const NiPixelFormat* kFormat,
            unsigned int uiMipmapLevels,
            unsigned int uiFaces) {
            gLog.FormattedMessage("\nCall NiPixelDataBuild");
            gLog.FormattedMessage("uiWidth: %u", uiWidth);
            gLog.FormattedMessage("uiHeight: %u", uiHeight);
            gLog.FormattedMessage("kFormat: %p", kFormat);
            gLog.FormattedMessage("uiMipmapLevels: %u", uiMipmapLevels);
            gLog.FormattedMessage("uiFaces: %u", uiFaces);
            NiPixelData* ret = ThisStdCall<NiPixelData*>(
                0xA7C190,
                this,
                uiWidth,
                uiHeight,
                kFormat,
                uiMipmapLevels,
                uiFaces);
            gLog.FormattedMessage("NiPixelDataBuild End\n");
            return ret;
        }
    };

    NiPixelData* NiPixelDataBuild2(
		NiPixelData* source,
        unsigned int uiWidth,
        unsigned int uiHeight,
        const NiPixelFormat* kFormat,
        unsigned int uiMipmapLevels,
        unsigned int uiFaces) {
        gLog.FormattedMessage("\nCall NiPixelDataBuild");
        gLog.FormattedMessage("uiWidth: %u", uiWidth);
        gLog.FormattedMessage("uiHeight: %u", uiHeight);
        gLog.FormattedMessage("kFormat: %p", kFormat);
        gLog.FormattedMessage("uiMipmapLevels: %u", uiMipmapLevels);
        gLog.FormattedMessage("uiFaces: %u", uiFaces);
        NiPixelData* ret = ThisStdCall<NiPixelData*>(
            0xA7C190,
            source,
            uiWidth,
            uiHeight,
            kFormat,
            uiMipmapLevels,
            uiFaces);
        gLog.FormattedMessage("NiPixelDataBuild End\n");
        return ret;
    }

    class FontEx : public Font {
    public:
        void Load()
        {
            NiTexturingProperty* NiTexturingProperty_4; // [esp+10h] [ebp-23Ch]
            NiPixelData* NiPixelData_1; // [esp+18h] [ebp-234h]
            float fMaxDrop_1; // [esp+20h] [ebp-22Ch]
            float fHeight_1; // [esp+24h] [ebp-228h]
            float fFontHeight_1; // [esp+28h] [ebp-224h]
            char* pFontFile_1; // [esp+58h] [ebp-1F4h]
            void* v8; // [esp+60h] [ebp-1ECh]
            unsigned int v9; // [esp+64h] [ebp-1E8h]
            int v10; // [esp+6Ch] [ebp-1E0h] BYREF
            unsigned int v11; // [esp+70h] [ebp-1DCh]
            unsigned int v12; // [esp+74h] [ebp-1D8h]
            DWORD v13[2]; // [esp+78h] [ebp-1D4h] BYREF
            bool m_pfnWrite; // [esp+83h] [ebp-1C9h]
            int savedTlsValueTemp; // [esp+84h] [ebp-1C8h]
            char* pFontFile; // [esp+88h] [ebp-1C4h]
            float fMaxDrop; // [esp+8Ch] [ebp-1C0h]
            float fMaxDrop_2; // [esp+90h] [ebp-1BCh]
            float fHeight; // [esp+94h] [ebp-1B8h]
            float fFontHeight; // [esp+98h] [ebp-1B4h]
            unsigned int v21; // [esp+9Ch] [ebp-1B0h]
            FontData* pFontData_1; // [esp+A0h] [ebp-1ACh]
            unsigned int v23; // [esp+A4h] [ebp-1A8h]
            unsigned int v24; // [esp+A8h] [ebp-1A4h]
            DWORD v25[2]; // [esp+ACh] [ebp-1A0h] BYREF
            char* pFontFile_2; // [esp+B4h] [ebp-198h]
            unsigned __int16 iRefCount; // [esp+BAh] [ebp-192h]
            int oldTlsValue; // [esp+BCh] [ebp-190h]
            int stringRefFlag; // [esp+C0h] [ebp-18Ch]
            BSFile* NiBinaryStream_3; // [esp+C4h] [ebp-188h]
            BSFile* NiBinaryStream_4; // [esp+C8h] [ebp-184h]
            NiFixedString kName_; // [esp+CCh] [ebp-180h] BYREF
            NiTexturingProperty* NiTexturingProperty_3; // [esp+D0h] [ebp-17Ch]
            NiTexturingProperty* NiTexturingProperty_1; // [esp+D4h] [ebp-178h]
            NiPixelData* v36; // [esp+D8h] [ebp-174h]
            NiPixelData* NiPixelData_2; // [esp+DCh] [ebp-170h]
            BSFile* NiBinaryStream_1; // [esp+E0h] [ebp-16Ch]
            BSFile* NiBinaryStream_2; // [esp+E4h] [ebp-168h]
            BSFile* NiFile_1; // [esp+E8h] [ebp-164h]
            BSFile* BSFile_2; // [esp+ECh] [ebp-160h]
            FontData* pFontData; // [esp+F0h] [ebp-15Ch]
            BSFile* BSFile_3; // [esp+F4h] [ebp-158h]
            BSFile* BSFile_4; // [esp+F8h] [ebp-154h]
            unsigned int a2; // [esp+FCh] [ebp-150h] BYREF
            unsigned int a3; // [esp+100h] [ebp-14Ch]
            NiTexture::FormatPrefs arPrefs_; // [esp+104h] [ebp-148h] BYREF
            BSFile* NiBinaryStream_0; // [esp+110h] [ebp-13Ch]
            NiTexturingProperty* NiTexturingProperty_2; // [esp+114h] [ebp-138h]
            NiPixelData* NiPixelData_3; // [esp+118h] [ebp-134h]
            int iTextureCount; // [esp+11Ch] [ebp-130h]
            float fFontHeight_2; // [esp+120h] [ebp-12Ch]
            int n256; // [esp+124h] [ebp-128h]
            char apName_[260]; // [esp+128h] [ebp-124h] BYREF
            float fWidth; // [esp+230h] [ebp-1Ch]
            BSFile* BSFile_1; // [esp+234h] [ebp-18h]
            float fHeight_2; // [esp+238h] [ebp-14h]
            int savedTlsValue; // [esp+23Ch] [ebp-10h]
            int stackCookie; // [esp+248h] [ebp-4h]

            DWORD tebAddress;
            DWORD tlsPointer;
            DWORD tlsSlotAddress;
            DWORD targetAddress;
            DWORD* pTlsIndex = (DWORD*)0x126FD98;

            gLog.FormattedMessage("Call Font::Load");

            __asm {
                mov eax, fs: [0x18]
                mov tebAddress, eax
            }
            tlsPointer = *(DWORD*)(tebAddress + 0x2C);
            tlsSlotAddress = *(DWORD*)(tlsPointer + (*pTlsIndex) * 4);
            targetAddress = tlsSlotAddress + 692;

            gLog.FormattedMessage("Set stringRefFlag");

            stringRefFlag = 0;

            oldTlsValue = *(DWORD*)targetAddress;
            savedTlsValue = oldTlsValue;
            *(DWORD*)targetAddress = 12;

            gLog.FormattedMessage("Initialize stackCookie");

            stackCookie = 0;
            iRefCount = this->iRefCount;
            if (iRefCount || !this->pFontFile)
            {
            LABEL_46:
                gLog.FormattedMessage("LABEL_46");
                ++this->iRefCount;
                stackCookie = -1;
                *(DWORD*)targetAddress = savedTlsValue;
                gLog.FormattedMessage("Font::Load End\n");
                return;
            }
            gLog.FormattedMessage("Load %s", (const char*)this->pFontFile);
            BSFile_1 = FileFinder_GetFile(this->pFontFile, 0, 0x155CC0u, 2u);
            gLog.FormattedMessage("Load %s End", (const char*)this->pFontFile);
            if (BSFile_1)
            {
                if (LOBYTE(BSFile_1->m_pFile))
                {
                    gLog.FormattedMessage("Allocate pFontData");
                    pFontData = (FontData*)MemoryManager_s_Instance->Allocate(0x3928u);
                    gLog.FormattedMessage("Allocate pFontData End");
                    this->pFontData = pFontData;
                    v21 = BSFile_1->GetSize();
                    pFontData_1 = this->pFontData;
                    v25[0] = 1;
                    v23 = BSFile_1->m_pfnRead(BSFile_1, pFontData_1, v21, v25, 1u);
                    BSFile_1->m_uiAbsoluteCurrentPos += v23;
                    v24 = v23;
                    NiFile_1 = BSFile_1;
                    //BSFile_2 = BSFile_1;
                    gLog.FormattedMessage("Delete BSFile_2");
                    if (BSFile_1) {
                        //delete(BSFile_2);
                    }
                    gLog.FormattedMessage("Delete BSFile_2 End");
                    BSFile_1 = 0;
                    this->fFontHeight = 0.0;
                    fHeight_2 = 0.0;
                    this->fMaxDrop = 0.0;
                    for (n256 = 0; n256 < 256; ++n256)
                    {
                        fFontHeight_2 = this->pFontData->fBaseLine - this->pFontData->pFontLetters[n256].fTopEdge;
                        fFontHeight_2 = fFontHeight_2 + this->pFontData->pFontLetters[n256].fHeight;
                        fFontHeight = this->fFontHeight;
                        if (fFontHeight_2 >= (double)fFontHeight)
                            fFontHeight_1 = fFontHeight_2;
                        else
                            fFontHeight_1 = fFontHeight;
                        this->fFontHeight = fFontHeight_1;
                        fHeight = this->pFontData->pFontLetters[n256].fHeight;
                        if (fHeight >= (double)fHeight_2)
                            fHeight_1 = fHeight;
                        else
                            fHeight_1 = fHeight_2;
                        fHeight_2 = fHeight_1;
                        fMaxDrop = this->fMaxDrop;
                        fMaxDrop_2 = this->pFontData->pFontLetters[n256].fTopEdge - this->pFontData->pFontLetters[n256].fHeight;
                        if (fMaxDrop_2 <= (double)fMaxDrop)
                            fMaxDrop_1 = fMaxDrop_2;
                        else
                            fMaxDrop_1 = fMaxDrop;
                        this->fMaxDrop = fMaxDrop_1;
                    }
                    fWidth = this->pFontData->pFontLetters[32].fWidth;
                    this->pFontData->pFontLetters[32].fWidth = this->pFontData->pFontLetters[32].fSpacing;
                    this->pFontData->pFontLetters[32].fSpacing = fWidth;
                    this->pFontData->pFontLetters[32].fHeight = fHeight_2;
                    this->pFontData->pFontLetters[32].fTopEdge = this->fMaxDrop + fHeight_2;
                    this->pFontData->pFontLetters[160].fWidth = this->pFontData->pFontLetters[32].fWidth;
                    this->pFontData->pFontLetters[160].fSpacing = this->pFontData->pFontLetters[32].fSpacing;
                    this->pFontData->pFontLetters[160].fHeight = this->pFontData->pFontLetters[32].fHeight;
                    this->pFontData->pFontLetters[160].fTopEdge = this->pFontData->pFontLetters[32].fTopEdge;
                    this->pFontData->pFontLetters[127].fWidth = this->pFontData->pFontLetters[124].fWidth;
                    this->pFontData->pFontLetters[127].fLeadingEdge = this->pFontData->pFontLetters[124].fLeadingEdge;
                    this->pFontData->pFontLetters[127].fSpacing = this->pFontData->pFontLetters[124].fSpacing;
                    this->pFontData->pFontLetters[127].fHeight = this->pFontData->pFontLetters[124].fHeight;
                    this->pFontData->pFontLetters[127].fTopEdge = this->pFontData->pFontLetters[124].fTopEdge;
                    this->pFontData->pFontLetters[0].fWidth = 0.0;
                    this->pFontData->pFontLetters[0].fSpacing = 0.0;
                    this->pFontData->pFontLetters[0].fHeight = fHeight_2;
                    this->pFontData->pFontLetters[0].fTopEdge = this->fMaxDrop + fHeight_2;
                    this->pFontData->pFontLetters[0].pMapping[0].fU = 0.0;
                    this->pFontData->pFontLetters[0].pMapping[1].fU = 0.0;
                    this->pFontData->pFontLetters[0].pMapping[2].fU = 0.0;
                    this->pFontData->pFontLetters[0].pMapping[3].fU = 0.0;
                    this->pFontData->pFontLetters[0].pMapping[0].fV = 0.0;
                    this->pFontData->pFontLetters[0].pMapping[1].fV = 0.0;
                    this->pFontData->pFontLetters[0].pMapping[2].fV = 0.0;
                    this->pFontData->pFontLetters[0].pMapping[3].fV = 0.0;
                    if (this->pFontData->iTextureCount > 8)
                    {
                        pFontFile = this->pFontFile;
                        stackCookie = -1;
                        savedTlsValueTemp = savedTlsValue;
                        *(DWORD*)targetAddress = savedTlsValue;
                        return;
                    }
                    for (iTextureCount = 0; iTextureCount < this->pFontData->iTextureCount; ++iTextureCount)
                    {
                        _snprintf_s(
                            apName_,
                            0x100u,
                            _TRUNCATE,
                            "TEXTURES\\FONTS\\%s.TEX",
                            this->pFontData->pTextureFiles[iTextureCount].pFilename
                        );

                        gLog.FormattedMessage("Load %s", (const char*)apName_);

                        NiBinaryStream_0 = FileFinder_GetFile((const char*)apName_, 0, 0x3E803E8u, 2u);

                        gLog.FormattedMessage("Load %s End", (const char*)apName_);

                        if (!NiBinaryStream_0 || !(m_pfnWrite = (bool)NiBinaryStream_0->m_pFile))
                        {
                            if (NiBinaryStream_0)
                            {
                                NiBinaryStream_1 = NiBinaryStream_0;
                                //NiBinaryStream_2 = NiBinaryStream_0;
                                gLog.FormattedMessage("Delete NiBinaryStream_2");
                                //delete(NiBinaryStream_2);
                                gLog.FormattedMessage("Delete NiBinaryStream_2 End");
                            }
                            stackCookie = -1;
                            v13[1] = savedTlsValue;
                            *(DWORD*)targetAddress = savedTlsValue;
                            return;
                        }

                        v13[0] = 1;
                        v11 = NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, &a2, 4u, v13, 1u);
                        v11 += NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, &a3, 4u, v13, 1u);
                        NiBinaryStream_0->m_uiAbsoluteCurrentPos += v11;
                        v12 = v11;
                        arPrefs_.m_ePixelLayout = static_cast<NiTexture::FormatPrefs::PixelLayout>(0x6);
                        arPrefs_.m_eAlphaFmt = static_cast<NiTexture::FormatPrefs::AlphaFormat>(0x3);
                        arPrefs_.m_eMipMapped = static_cast<NiTexture::FormatPrefs::MipFlag>(0x2);
                        v36 = (NiPixelData*)NiMemObject::operator new(0x74);
                        stackCookie = (stackCookie & 0xFFFFFF00) | 1;
                        if (v36){
                            gLog.FormattedMessage("Instancing NiPixelData");
                            NiPixelData_1 = NiPixelDataBuild2(
                                v36,
                                a2,
                                a3,
                                reinterpret_cast<const NiPixelFormat*>(0x11AA2A0),
                                1,
                                1);
                            gLog.FormattedMessage("Instancing NiPixelData End");
                        }
                        else
                            NiPixelData_1 = 0;
                        NiPixelData_2 = NiPixelData_1;
                        stackCookie = (stackCookie & 0xFFFFFF00) | 0;
                        NiPixelData_3 = NiPixelData_1;
                        v8 = &NiPixelData_1->m_pucPixels[*NiPixelData_1->m_puiOffsetInBytes];
                        v10 = 1;
                        v9 = NiBinaryStream_0->m_pfnRead(NiBinaryStream_0, v8, 4 * a3 * a2, (UInt32*)&v10, 1u);
                        NiBinaryStream_0->m_uiAbsoluteCurrentPos += v9;
                        NiPixelData_3->bNoConvert = 1;
                        NiTexturingProperty_3 = (NiTexturingProperty*)NiMemObject::operator new(0x30);
                        stackCookie = (stackCookie & 0xFFFFFF00) | 2;
                        if (NiTexturingProperty_3)
                        {
                            pFontFile_1 = this->pFontFile;
                            if (pFontFile_1)
                                kName_.m_kHandle = (char*)NiGlobalStringTable::AddString(pFontFile_1);
                            else
                                kName_.m_kHandle = 0;
                            stackCookie = (stackCookie & 0xFFFFFF00) | 3;
                            stringRefFlag |= 1u;
                            gLog.FormattedMessage("Instancing NiTexturingProperty");
                            /*NiTexturingProperty_4 = static_cast<NiTexturingPropertyEx*>(NiTexturingProperty_3)->NiTexturingPropertyBuild(
                                NiPixelData_3,
                                &kName_,
                                &arPrefs_);*/
                            NiTexturingProperty_4 = NiTexturingPropertyBuild2(
                                NiTexturingProperty_3,
                                NiPixelData_3,
                                &kName_,
                                &arPrefs_);
                            gLog.FormattedMessage("Instancing NiTexturingProperty End");
                        }
                        else
                        {
                            NiTexturingProperty_4 = 0;
                        }
                        NiTexturingProperty_1 = NiTexturingProperty_4;
                        NiTexturingProperty_2 = NiTexturingProperty_4;
                        stackCookie = 0;
                        if ((stringRefFlag & 1) != 0)
                        {
                            stringRefFlag &= ~1u;
                            if (kName_.m_kHandle)
                                InterlockedDecrement((volatile LONG*)kName_.m_kHandle - 2);
                        }
                        NiBinaryStream_3 = NiBinaryStream_0;
                        //NiBinaryStream_4 = NiBinaryStream_0;
                        if (NiBinaryStream_0) {
                            gLog.FormattedMessage("Delete NiBinaryStream_4");
                            //delete(NiBinaryStream_4);
                            gLog.FormattedMessage("Delete NiBinaryStream_4 End");
                        }
                        NiBinaryStream_0 = 0;
                        ThisStdCall(
                            0x60AEB0,
                            NiTexturingProperty_2,
                            1);
                        ThisStdCall(
                            0x66B0D0,
                            &this->pTextureData[iTextureCount].m_pObject,
                            (int)NiTexturingProperty_2);
                        gLog.FormattedMessage("finish load");
                    }
                    goto LABEL_46;
                }
            }
            pFontFile_2 = this->pFontFile;
            if (BSFile_1)
            {
                BSFile_3 = BSFile_1;
                //BSFile_4 = BSFile_1;
                gLog.FormattedMessage("Delete BSFile_4");
                //delete(BSFile_4);
                gLog.FormattedMessage("Delete BSFile_4 End");
            }
            stackCookie = -1;
            v25[1] = savedTlsValue;
            *(DWORD*)targetAddress = savedTlsValue;
        }
    };

    FontInfo::GlyphInfo* RenderCharacterGlyphEx1(
        FontInfo::GlyphInfo* glyphInfo,
        UInt32 aiVert,
        NiTriShape* apShape,
        float* axPos,
        const NiColorA* apColor
    ) {
        gLog.FormattedMessage("From location 1");
        gLog.FormattedMessage("iTextureIndex = %f", glyphInfo->unk00);
        gLog.FormattedMessage("UV topLeft = %f", glyphInfo->topLeft);
        gLog.FormattedMessage("UV topRight = %f", glyphInfo->topRight);
        gLog.FormattedMessage("UV bottomLeft = %f", glyphInfo->bottomLeft);
        gLog.FormattedMessage("UV bottomRightt = %f", glyphInfo->bottomRight);
        gLog.FormattedMessage("fWidth = %f", glyphInfo->width);
        gLog.FormattedMessage("fHeight = %f", glyphInfo->height);
        gLog.FormattedMessage("fLeadingEdge = %f", glyphInfo->kerningLeft);
        gLog.FormattedMessage("fSpacing = %f", glyphInfo->kerningRight);
        gLog.FormattedMessage("fTopEdge = %f", glyphInfo->ascent);

        FontInfo::GlyphInfo* ret = StdCall<FontInfo::GlyphInfo*>(
            0xA142D0,
            glyphInfo,
            aiVert,
            apShape,
            axPos,
            apColor);
        return ret;
    }

    FontInfo::GlyphInfo* RenderCharacterGlyphEx2(
        FontInfo::GlyphInfo* glyphInfo,
        UInt32 aiVert,
        NiTriShape* apShape,
        float* axPos,
        const NiColorA* apColor
    ) {
        gLog.FormattedMessage("From location 2");
        gLog.FormattedMessage("iTextureIndex = %f", glyphInfo->unk00);
        gLog.FormattedMessage("UV topLeft = %f", glyphInfo->topLeft);
        gLog.FormattedMessage("UV topRight = %f", glyphInfo->topRight);
        gLog.FormattedMessage("UV bottomLeft = %f", glyphInfo->bottomLeft);
        gLog.FormattedMessage("UV bottomRightt = %f", glyphInfo->bottomRight);
        gLog.FormattedMessage("fWidth = %f", glyphInfo->width);
        gLog.FormattedMessage("fHeight = %f", glyphInfo->height);
        gLog.FormattedMessage("fLeadingEdge = %f", glyphInfo->kerningLeft);
        gLog.FormattedMessage("fSpacing = %f", glyphInfo->kerningRight);
        gLog.FormattedMessage("fTopEdge = %f", glyphInfo->ascent);

        FontInfo::GlyphInfo* ret = StdCall<FontInfo::GlyphInfo*>(
            0xA142D0,
            glyphInfo,
            aiVert,
            apShape,
            axPos,
            apColor);
        return ret;
    }

    FontInfo::GlyphInfo* RenderCharacterGlyphEx3(
        FontInfo::GlyphInfo* glyphInfo,
        UInt32 aiVert,
        NiTriShape* apShape,
        float* axPos,
        const NiColorA* apColor
    ) {
        gLog.FormattedMessage("From location 3");
        gLog.FormattedMessage("iTextureIndex = %f", glyphInfo->unk00);
        gLog.FormattedMessage("UV topLeft = %f", glyphInfo->topLeft);
        gLog.FormattedMessage("UV topRight = %f", glyphInfo->topRight);
        gLog.FormattedMessage("UV bottomLeft = %f", glyphInfo->bottomLeft);
        gLog.FormattedMessage("UV bottomRightt = %f", glyphInfo->bottomRight);
        gLog.FormattedMessage("fWidth = %f", glyphInfo->width);
        gLog.FormattedMessage("fHeight = %f", glyphInfo->height);
        gLog.FormattedMessage("fLeadingEdge = %f", glyphInfo->kerningLeft);
        gLog.FormattedMessage("fSpacing = %f", glyphInfo->kerningRight);
        gLog.FormattedMessage("fTopEdge = %f", glyphInfo->ascent);

        FontInfo::GlyphInfo* ret = StdCall<FontInfo::GlyphInfo*>(
            0xA142D0,
            glyphInfo,
            aiVert,
            apShape,
            axPos,
            apColor);
        return ret;
    }

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

        char __thiscall CreateTextEx(BSString a2, int a3, int a4) {
            gLog.FormattedMessage("CreateTextEx");
            char ret = ThisStdCall<char>(0xA18F00, this, a2, a3, a4);
            return ret;
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

            //gLog.Message("Call GetStringDimensionsEx");

            if (fontID >= 1 && fontID <= 8 && srcString)
            {
                StringDimensions = StringDefaulDimensions;
                // 0xEC6130
                //gLog.Message("Call strlen");
                sourceStringLength = strlen(srcString);
                //gLog.FormattedMessage("sourceStringLength: %u", sourceStringLength);
                fontCharMetrics = this->fontInfos[fontID - 1]->fontData->glyphs;
                lastValidWrapPosition = 0.0;
                currentLineWidth = 0.0;
                //gLog.Message("Call VertSpacingAdjust");
                fontVerticalSpacingAdjust = GetFontVerticalSpacingAdjustment(fontID);
                //gLog.FormattedMessage("fontVerticalSpacingAdjust: %u", fontVerticalSpacingAdjust);
                previousCharTotalWidth = 0.0;
                hasHyphenationPoint = 0;
                totalLines = 1;
                StringDimensions.y = fontCharMetrics[' '].height;
                for (currentCharIndex = startCharIndex; currentCharIndex < sourceStringLength; ++currentCharIndex)
                {
                    currentChar = srcString[currentCharIndex];
                    currentCharTotalWidth = 0.0;
                    //gLog.Message("Call ConvertToAsciiQuotes");
                    ConvertToAsciiQuotes(&currentChar);
                    //gLog.Message("Call ConvertToAsciiQuotes end");
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

    class FontTextReplacedEx : public FontTextReplaced {
    public:
        bool StringSet(const char* apText, UINT32 auiLength) {
            //gLog.FormattedMessage("apText = '%s'", apText);
            bool ret = ThisStdCall<bool>(0x4037F0, this, apText, auiLength);
            return ret;
        }
    };

	void InitVertSpacingHook() {
        WriteRelJump(0xA1B3A0, &VertSpacingAdjust);
	}

    void InitFontHook() {
        // FontManager::GetStringDimensions
        //WriteRelJumpEx(0xA1B020, &FontManagerEx::GetStringDimensionsEx);
        // 
        // FontInfo::CalculateTextLayout
        //WriteRelJumpEx(0xA12FB0, &FontInfoEx::CalculateTextLayoutEx);
        // 
        // Font::Load
        //WriteRelCallEx(0xA1219D, &FontInfoEx::LoadFontDataEx);
        WriteRelCallEx(0xA1219D, &FontEx::Load);
        // 
        // FontInfo::GenerateTextGeometry
        //WriteRelCallEx(0xA22211, &FontInfoEx::GenerateTextGeometryEx);
        // 
        // FontTextReplaced::StringSet
        //WriteRelCallEx(0xA21D64, &FontTextReplacedEx::StringSet);
        // 
        // RenderCharacterGlyph
        //WriteRelCall(0xA1278B, &RenderCharacterGlyphEx1);
        //WriteRelCall(0xA12E1B, &RenderCharacterGlyphEx2);
        //WriteRelCall(0xA19622, &RenderCharacterGlyphEx3);
        // 
        //FontManager::CreateText
        //WriteRelCallEx(0xA220C6, &FontManagerEx::CreateTextEx);
        //
        //NiTexturingProperty::NiTexturingProperty
        //WriteRelCallEx(0xA15D86, &NiTexturingPropertyEx::NiTexturingPropertyBuild);

        //NiPixelData::NiPixelData
        //WriteRelCallEx(0xA15C03, &NiPixelDataEx::NiPixelDataBuild);
    }
}