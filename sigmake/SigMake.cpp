#include "stdafx.h"

const static duint CodeSizeMinimum = 5;            // 005 bytes
const static duint CodeSizeMaximum = 128 * 1024;// 128 kilobytes

SIG_DESCRIPTOR *GenerateSigFromCode(duint Start, duint End)
{
    //
    // Avoid duplicating code everywhere
    //
    bool returnStatus = false;

    PBYTE processMemory        = nullptr;
    SIG_DESCRIPTOR *desc    = nullptr;
    _DInst *instructions    = nullptr;

    //
    // Check if the copy size is within sane limits.
    // The last byte is inclusive.
    //
    duint codeSize = End - Start + 1;

    if (codeSize < CodeSizeMinimum || codeSize > CodeSizeMaximum)
    {
        _plugin_printf("Code size 0x%X not within bounds (Min: 0x%X Max: 0x%X)\n", codeSize, CodeSizeMinimum, CodeSizeMaximum);
        goto __freememory;
    }

    //
    // Allocate and read the memory buffer
    //
    processMemory = (PBYTE)BridgeAlloc(codeSize);

    if (!DbgMemRead(Start, processMemory, codeSize))
    {
        _plugin_printf("Couldn't read process memory\n");
        goto __freememory;
    }

    //
    // Allocate the descriptor buffer (Count = # of bytes)
    //
    desc = AllocDescriptor((ULONG)codeSize);

    //
    // Allocate the disassembly buffer
    //
    uint32_t instructionCount    = 0;
    instructions                = (_DInst *)BridgeAlloc(codeSize * sizeof(_DInst));

    //
    // Decode the assembly
    //
    _CodeInfo info;
    info.codeOffset = Start;
    info.code        = processMemory;
    info.codeLen    = codeSize;
    info.features    = DF_NONE;

#ifdef _WIN64
    info.dt = Decode64Bits;
#else
    info.dt = Decode32Bits;
#endif // _WIN64

    _DecodeResult res = distorm_decompose(&info, instructions, codeSize, &instructionCount);

    // Check if decoding failed
    if (res != DECRES_SUCCESS)
    {
        _plugin_printf("Instruction decoding failed\n");
        goto __freememory;
    }

    //
    // Loop through each instruction
    //
    uint32_t i = 0;// Instruction index counter
    uint32_t d = 0;// Data index counter

    for (; i < instructionCount; i++)
    {
        // Determine if the bytes should be used or not
        // All bytes are a wild card unless if proved otherwise
        {
            int matchSize = MatchInstruction(&instructions[i], &processMemory[d]);

            // Copy the actual instruction data into signature format
            for (int j = 0; j < instructions[i].size; j++)
            {
                // Is 'j' within the matched data block?
                if (j < matchSize)
                {
                    desc->Entries[d + j].Value        = processMemory[d + j];
                    desc->Entries[d + j].Wildcard    = 0;
                }
                else
                {
                    desc->Entries[d + j].Value        = 0;
                    desc->Entries[d + j].Wildcard    = 1;
                }
            }

            d += instructions[i].size;
        }
    }

    //
    // Is the setting enabled to trim signatures?
    //
    if (Settings::TrimSignatures)
        TrimDescriptor(desc);

    //
    // Is the setting enabled to shorten signatures?
    //
    if (Settings::ShortestSignatures)
        ShortenDescriptor(desc);

    returnStatus = true;

__freememory:
    //
    // Free the memory
    //
    if (processMemory)
        BridgeFree(processMemory);

    if (instructions)
        BridgeFree(instructions);

    //
    // Was this function successful?
    //
    if (!returnStatus)
    {
        if (desc)
            BridgeFree(desc);

        return nullptr;
    }

    return desc;
}

void PatternScan(SIG_DESCRIPTOR *Descriptor, std::vector<duint>& Results)
{
    //
    // Verify
    //
    if (Descriptor->Count <= 0)
    {
        _plugin_printf("Trying to scan with an invalid signature\n");
        return;
    }

    //
    // Get a copy of the current module in disassembly
    //
    duint moduleBase    = DbgGetCurrentModule();
    duint moduleSize    = DbgFunctions()->ModSizeFromAddr(moduleBase);
    PBYTE processMemory = (PBYTE)BridgeAlloc(moduleSize);

    if (!DbgMemRead(moduleBase, processMemory, moduleSize))
    {
        _plugin_printf("Couldn't read process memory for scan\n");
        return;
    }

    //
    // Compare function
    //
    auto DataCompare = [](PBYTE Data, SIG_DESCRIPTOR_ENTRY *Entries, ULONG Count)
    {
        ULONG i = 0;

        for (; i < Count; ++Data, ++i)
        {
            if (Entries[i].Wildcard == 0 && *Data != Entries[i].Value)
                return false;
        }

        return i == Count;
    };

    //
    // Scanner loop
    //
    for (duint i = 0; i < moduleSize; i++)
    {
        PBYTE dataAddr = processMemory + i;

        if (DataCompare(dataAddr, Descriptor->Entries, Descriptor->Count))
            Results.push_back(moduleBase + i);
    }
}

bool MatchOperands(_DInst *Instruction, _Operand *Operands, int PrefixSize)
{
    //
    // This function determines if an instruction is static and will be
    // included in the signature. Each operand is checked (4) to verify this.
    // Settings are also taken into account.
    //

    //
    // Determine if short branches are allowed
    //
    if (META_GET_FC(Instruction->meta) == FC_UNC_BRANCH ||
        META_GET_FC(Instruction->meta) == FC_CND_BRANCH)
    {
        //
        // Unused prefixes might cause a larger instruction size
        //
        if (Settings::IncludeShortJumps && ((Instruction->size - PrefixSize) < 5))
            return true;
    }

    //
    // Loop through the operands
    //
    for (int i = 0; i < ARRAYSIZE(Instruction->ops); i++)
    {
        switch (Operands[i].type)
        {
        case O_NONE:    // Invalid operand
        case O_REG:        // Register
            continue;

        case O_IMM:        // Only accept IMM if it's less than 32 bits
            if (Settings::IncludeMemRefences || Operands[i].size < 32)
                continue;
            return false;

        case O_IMM1:    // Special operands for ENTER (These are INCLUDED)
        case O_IMM2:    // Same as above
            continue;

        case O_DISP:    // Only accept DISP if it's less than 32 bits,
        case O_SMEM:    // or if it is RIP-relative
        case O_MEM:        //
            if (Settings::IncludeMemRefences || Instruction->dispSize < 32)
                continue;

#ifdef _WIN64
            if (Settings::IncludeRelAddresses && Operands[i].index == R_RIP)
                continue;
#endif // _WIN64

            return false;

        case O_PC:        // Relative branches
        case O_PTR:        // FAR branches
            return false;
        }
    }

    return true;
}

int MatchInstruction(_DInst *Instruction, PBYTE Data)
{
    //
    // Are wild cards forced to be off?
    //
    if (Settings::DisableWildcards)
        return Instruction->size;

    //
    // Create a temporary struct in order to decode data
    //
    _CodeInfo info;
    _PrefixState ps;

    memset(&info, 0, sizeof(_CodeInfo));
    memset(&ps, 0, sizeof(_PrefixState));
    memset(ps.pfxIndexer, PFXIDX_NONE, sizeof(int) * PFXIDX_MAX);

    info.codeOffset = (_OffsetType)Data;
    info.code        = Data;
    info.codeLen    = Instruction->size;
    info.features    = DF_NONE;

    ps.start        = Data;
    ps.last            = Data;

#ifdef _WIN64
    info.dt = Decode64Bits;
#else
    info.dt = Decode32Bits;
#endif // _WIN64

    //
    // Calculate the prefixes (already validated)
    // along with the sizes.
    //
    prefixes_decode(Data, info.codeLen, &ps, info.dt);

    int prefixSize = (int)(ps.start - ps.last);

    //
    // The return value is ignored here. _CodeInfo::codeLen is modified
    // and varies depending on the instruction length.
    //
    info.codeOffset = (_OffsetType)ps.last;
    info.code        = ps.last;
    info.codeLen    -= prefixSize;

    inst_lookup(&info, &ps);

    //
    // The instruction opcode itself is ALWAYS returned. The
    // operands are variable.
    // 'partialInstructionSize' holds sizeof(PREFIX) + sizeof(OPCODE).
    // 'totalInstructionSize'   holds sizeof(PREFIX) + sizeof(OPCODE) + sizeof(OPERANDS).
    //
    int partialInstructionSize    = Instruction->size - info.codeLen;
    int totalInstructionSize    = Instruction->size;

    //
    // Determine if the operands should be included - increasing
    // the total copy size.
    //
    if (MatchOperands(Instruction, Instruction->ops, prefixSize))
        return totalInstructionSize;

    return partialInstructionSize;
}
