#pragma once
#include <cstring>
#include <cstdint>
#include <cstring>
#include <limits>
#include "SafeWrite.h"
#include "Game/uidecode.h"
#include "Game/Bethesda/MemoryManager.hpp"
#include "tnvse.h"

namespace tNVSE {
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

    MemoryManager* TextMemoryManagerInstance = reinterpret_cast<MemoryManager*>(0x11F6238);

    class FontInfoEx : public FontInfo {
    public:
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
            originalTextBuffer = static_cast<char*>(TextMemoryManagerInstance->Allocate(sourceTextLen + 4));
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
            processedTextBuffer = static_cast<char*>(TextMemoryManagerInstance->Allocate(sourceTextLen + 4));
            if (!processedTextBuffer) {
                //gLog.Message("Memory allocation failed for processedTextBuffer");
                TextMemoryManagerInstance->Deallocate(originalTextBuffer);
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
                            dynamicTextBuffer = static_cast<char*>(TextMemoryManagerInstance->Reallocate(dynamicTextBuffer, textBufferSize));
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
                processedOriginalText = static_cast<char*>(TextMemoryManagerInstance->Reallocate(processedOriginalText, processedTextLen + 4));
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
                        dynamicTextBuffer = static_cast<char*>(TextMemoryManagerInstance->Reallocate(dynamicTextBuffer, processedTextLen + 4));
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
                        if (buttonIconIndex < this->buttonIconArray.size)
                        {
                            pCurrentGlyph->width = this->buttonIconArray.data[buttonIconIndex].unk01;
                            pCurrentGlyph->kerningRight = this->buttonIconArray.data[buttonIconIndex].unk04;
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
                                dynamicTextBuffer = static_cast<char*>(TextMemoryManagerInstance->Reallocate(dynamicTextBuffer, textBufferSize));
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
                                dynamicTextBuffer = static_cast<char*>(TextMemoryManagerInstance->Reallocate(dynamicTextBuffer, processedTextLen + 6));
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
                        dynamicTextBuffer = static_cast<char*>(TextMemoryManagerInstance->Reallocate(dynamicTextBuffer, processedTextLen + 4));
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
            TextMemoryManagerInstance->Deallocate(processedOriginalText);
            TextMemoryManagerInstance->Deallocate(dynamicTextBuffer);
            //gLog.Message("CalculateTextLayoutEx End");
        }

        void LoadFontDataEx() {
            NiTexturingProperty* resourceHandleTemp2; // [esp+10h] [ebp-23Ch]
            NiPixelData* texturingPropertyTemp2; // [esp+18h] [ebp-234h]
            float newMaxWidthMod; // [esp+20h] [ebp-22Ch]
            float newMaxGlyphHeight; // [esp+24h] [ebp-228h]
            float newMaxHeight; // [esp+28h] [ebp-224h]
            char* fontFilePathTemp; // [esp+58h] [ebp-1F4h]
            void* textureDataSize; // [esp+60h] [ebp-1ECh]
            UInt32 bytesRead3; // [esp+64h] [ebp-1E8h]
            UInt32 readFlag; // [esp+6Ch] [ebp-1E0h] BYREF
            UInt32 bytesRead1Temp; // [esp+70h] [ebp-1DCh]
            UInt32 bytesRead1; // [esp+74h] [ebp-1D8h]
            UInt32 readParams1[2]; // [esp+78h] [ebp-1D4h] BYREF
            bool isTexValid; // [esp+83h] [ebp-1C9h]
            UInt32 savedTlsValueTemp; // [esp+84h] [ebp-1C8h]
            const char* fontFilePath; // [esp+88h] [ebp-1C4h]
            float currentMaxWidthMod; // [esp+8Ch] [ebp-1C0h]
            float currentWidthMod; // [esp+90h] [ebp-1BCh]
            float currentGlyphHeight; // [esp+94h] [ebp-1B8h]
            float currentMaxHeight; // [esp+98h] [ebp-1B4h]
            UInt32 fileSize; // [esp+9Ch] [ebp-1B0h]
            FontInfo::BufferData* fontBuffer; // [esp+A0h] [ebp-1ACh]
            UInt32 bytesRead2Temp; // [esp+A4h] [ebp-1A8h]
            UInt32 bytesRead2; // [esp+A8h] [ebp-1A4h]
            UInt32 readParams2[2]; // [esp+ACh] [ebp-1A0h] BYREF
            char* fontFilePathError; // [esp+B4h] [ebp-198h]
            __int16 isLoadedFlag; // [esp+BAh] [ebp-192h]
            UInt32 oldTlsValue; // [esp+BCh] [ebp-190h]
            int stringRefFlag; // [esp+C0h] [ebp-18Ch]
            BSFile* texFileHandleTemp; // [esp+C4h] [ebp-188h]
            BSFile* texFileVTable; // [esp+C8h] [ebp-184h]
            String* fontPathCopy; // [esp+CCh] [ebp-180h] BYREF
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
            SInt32 texIndex; // [esp+11Ch] [ebp-130h]
            float glyphTotalHeight; // [esp+120h] [ebp-12Ch]
            UInt32 glyphIndex; // [esp+124h] [ebp-128h]
            char fontTexPath[260];; // [esp+128h] [ebp-124h] BYREF
            float tempWidth; // [esp+230h] [ebp-1Ch]
            BSFile* fntFileHandle; // [esp+234h] [ebp-18h]
            float maxGlyphHeight; // [esp+238h] [ebp-14h]
            UInt32 savedTlsValue; // [esp+23Ch] [ebp-10h]
            int stackCookie; // [esp+248h] [ebp-4h]

            DWORD tebAddress;
            DWORD tlsPointer;
            DWORD tlsSlotAddress;
            DWORD targetAddress;
            DWORD* pTlsIndex = (DWORD*)0x0126FD98;

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
                gLog.Message("Call LoadFontData End");
                return;
            }
            gLog.Message("Load File 1");
            fntFileHandle = LoadFile(this->filePath, 0, 0x4000u, 2);
            gLog.Message("Load File 1 end");
            if (fntFileHandle)
            {
                gLog.Message("fnt file valid");
                if (fntFileHandle->m_good)
                {
                    gLog.Message("Allocate font buffer data");
                    bufferData = static_cast<FontInfo::BufferData*>(TextMemoryManagerInstance->Allocate(0x3928u));
                    this->fontData = bufferData;
                    gLog.Message("Unk_0A");
                    // fileSize = fntFileHandle->Unk_0A(fntFileHandle);
                    fileSize = GetFileSize(fntFileHandle);
                    gLog.Message("Unk_0A finish");
                    fontBuffer = this->fontData;
                    readParams2[0] = 1;

                    gLog.Message("m_readProc");
                    //m_readProc
                    typedef UInt32(__cdecl* ReadProcType)(
                        BSFile* fileHandle,
                        void* buffer,
                        UInt32 size,
                        UInt32* readParams,
                        bool flag
                        );
                    ReadProcType pReadProc = *(ReadProcType*)((uintptr_t)fntFileHandle + 8);

                    bytesRead2Temp = pReadProc(
                        fntFileHandle,
                        fontBuffer,
                        fileSize,
                        readParams2,
                        true
                    );

                    //m_offset
                    UInt32* pOffset = reinterpret_cast<UInt32*>(
                        reinterpret_cast<uintptr_t>(fntFileHandle) + 4
                        );
                    *pOffset += bytesRead2Temp;

                    bytesRead2 = bytesRead2Temp;
                    File_1 = fntFileHandle;
                    FileVTable2 = fntFileHandle;
                    if (fntFileHandle)
                        FileVTable2->Destructor(1);
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
                    if (this->fontData->numTextures > 8)
                    {
                        fontFilePath = this->filePath;
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

                        if (!texFileHandle || !(isTexValid = texFileHandle->m_good))
                        {
                            gLog.Message("Tex not valid");
                            // 0x5B5E40 Return 0
                            if (texFileHandle)
                            {
                                texFileHandleTemp2 = texFileHandle;
                                texFileHandleVTable = texFileHandle;
                                texFileHandleVTable->Destructor(1);
                            }
                            stackCookie = -1;
                            readParams1[1] = savedTlsValue;
                            *(DWORD*)targetAddress = savedTlsValue;
                            return;
                        }
                        readParams1[0] = 1;

                        // m_blockName.str
                        typedef UInt32(*ReadFuncType)(void* a1, void* a2, UInt32 a3, UInt32* a4, UInt32 a5);
                        ReadFuncType readFunc = *reinterpret_cast<ReadFuncType*>(
                            reinterpret_cast<char*>(texFileHandle) + 8
                            );
                        bytesRead1Temp = readFunc(texFileHandle, &texWidth, 8, readParams1, 1);

                        gLog.Message("m_offset 2");
                        // m_offset
                        UInt32* pOffset2 = reinterpret_cast<UInt32*>(
                            reinterpret_cast<uintptr_t>(texFileHandle) + 4
                            );
                        *pOffset2 += bytesRead1Temp;
                        gLog.Message("m_offset 2 end");

                        bytesRead1 = bytesRead1Temp;
                        textureCreateArgs[0] = 6;
                        textureCreateArgs[1] = 3;
                        textureCreateArgs[2] = 2;

                        fontTextureObject = CdeclCall<NiPixelData*>(0xAA13E0, 116);

                        stackCookie = (stackCookie & 0xFFFFFF00) | 1;
                        if (fontTextureObject)
                            // NiPixelData::Init
                            texturingPropertyTemp2 = ThisStdCall<NiPixelData*>(
                                0xA7C190,
                                fontTextureObject,
                                texWidth,
                                texHeight,
                                reinterpret_cast<const void*>(0x11AA2A0),
                                1,
                                1
                            );
                        else
                            texturingPropertyTemp2 = 0;
                        fontTexturingPropertyTemp = texturingPropertyTemp2;
                        stackCookie = (stackCookie & 0xFFFFFF00) | 0;
                        texturingProperty = texturingPropertyTemp2;
                        textureDataSize = &texturingPropertyTemp2->m_pucPixels[*texturingPropertyTemp2->m_puiOffsetInBytes];
                        readFlag = 1;
                        bytesRead3 = readFunc(
                            texFileHandle,
                            textureDataSize,
                            4 * texHeight * texWidth,
                            &readFlag,
                            1);

                        gLog.Message("m_offset 3");
                        // m_offset
                        pOffset2 = reinterpret_cast<UInt32*>(
                            reinterpret_cast<uintptr_t>(texFileHandle) + 4
                            );
                        *pOffset2 += bytesRead3;
                        gLog.Message("m_offset 3 end");

                        texturingProperty->unk70 = (texturingProperty->unk70 & 0xFF00) | 0x0001;

                        resourceTemp = CdeclCall<NiTexturingProperty*>(0xAA13E0, 48);

                        stackCookie = (stackCookie & 0xFFFFFF00) | 2;
                        gLog.Message("Check resources");
                        if (resourceTemp)
                        {
                            gLog.Message("ResourceAvailable");
                            fontFilePathTemp = this->filePath;
                            if (fontFilePathTemp)
                                fontPathCopy = CdeclCall<String*>(0xA5B690, fontFilePathTemp);
                            else
                                fontPathCopy = 0;
                            stackCookie = (stackCookie & 0xFFFFFF00) | 3;
                            stringRefFlag |= 1u;
                            resourceHandleTemp2 = ThisStdCall<NiTexturingProperty*>(0xA6ABB0, resourceTemp, texturingProperty, &fontPathCopy, textureCreateArgs);
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
                                volatile LONG* refCount = reinterpret_cast<volatile LONG*>(&fontPathCopy[-1]);
                                InterlockedDecrement(refCount);
                            }
                        }
                        texFileHandleTemp = texFileHandle;
                        texFileVTable = texFileHandle;
                        if (texFileHandle)
                            texFileVTable->Destructor(1);
                        texFileHandle = 0;
                        ThisStdCall(0x60AEB0, resourceHandle, 1);
                        ThisStdCall(0x66B0D0, &this->fontTexProp + texIndex, resourceHandle);
                        gLog.Message("finish load");
                    }
                    goto LABEL_46;
                }
            }
            fontFilePathError = this->filePath;
            // 0x5B5E40 Return 0
            if (fntFileHandle)
            {
                File_3 = fntFileHandle;
                FileVTable1 = fntFileHandle;
                FileVTable1->Destructor(1);
            }
            stackCookie = -1;
            readParams2[1] = savedTlsValue;
            *(DWORD*)targetAddress = savedTlsValue;
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

	void InitVertSpacingHook() {
        WriteRelJump(0xA1B3A0, &VertSpacingAdjust);
	}

    void InitFontHook() {
        // FontManager::GetStringDimensions
        WriteRelJumpEx(0xA1B020, &FontManagerEx::GetStringDimensionsEx);
        // FontInfo::CalculateTextLayout
        WriteRelJumpEx(0xA12FB0, &FontInfoEx::CalculateTextLayoutEx);
        // FontInfo::LoadFontData
        WriteRelJumpEx(0xA15320, &FontInfoEx::LoadFontDataEx);
    }
}