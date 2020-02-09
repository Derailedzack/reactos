/*
 * COPYRIGHT:       See COPYING in the top level directory
 * PROJECT:         ReactOS Console Driver DLL
 * FILE:            win32ss/user/winsrv/consrv/condrv/text.c
 * PURPOSE:         Console Output Functions for text-mode screen-buffers
 * PROGRAMMERS:     Jeffrey Morlan
 *                  Hermes Belusca-Maito (hermes.belusca@sfr.fr)
 */

/* INCLUDES *******************************************************************/

#include <consrv.h>

#define NDEBUG
#include <debug.h>

/* GLOBALS ********************************************************************/

/*
 * From MSDN:
 * "The lpMultiByteStr and lpWideCharStr pointers must not be the same.
 *  If they are the same, the function fails, and GetLastError returns
 *  ERROR_INVALID_PARAMETER."
 */
#define ConsoleOutputUnicodeToAnsiChar(Console, dChar, sWChar) \
do { \
    ASSERT((ULONG_PTR)(dChar) != (ULONG_PTR)(sWChar)); \
    WideCharToMultiByte((Console)->OutputCodePage, 0, (sWChar), 1, (dChar), 1, NULL, NULL); \
} while (0)

#define ConsoleOutputAnsiToUnicodeChar(Console, dWChar, sChar) \
do { \
    ASSERT((ULONG_PTR)(dWChar) != (ULONG_PTR)(sChar)); \
    MultiByteToWideChar((Console)->OutputCodePage, 0, (sChar), 1, (dWChar), 1); \
} while (0)

/* PRIVATE FUNCTIONS **********************************************************/

CONSOLE_IO_OBJECT_TYPE
TEXTMODE_BUFFER_GetType(PCONSOLE_SCREEN_BUFFER This)
{
    // return This->Header.Type;
    return TEXTMODE_BUFFER;
}

static CONSOLE_SCREEN_BUFFER_VTBL TextVtbl =
{
    TEXTMODE_BUFFER_GetType,
};


/*static*/ VOID
ClearLineBuffer(PTEXTMODE_SCREEN_BUFFER Buff);


NTSTATUS
CONSOLE_SCREEN_BUFFER_Initialize(OUT PCONSOLE_SCREEN_BUFFER* Buffer,
                                 IN PCONSOLE Console,
                                 IN PCONSOLE_SCREEN_BUFFER_VTBL Vtbl,
                                 IN SIZE_T Size);
VOID
CONSOLE_SCREEN_BUFFER_Destroy(IN OUT PCONSOLE_SCREEN_BUFFER Buffer);


NTSTATUS
TEXTMODE_BUFFER_Initialize(OUT PCONSOLE_SCREEN_BUFFER* Buffer,
                           IN PCONSOLE Console,
                           IN HANDLE ProcessHandle,
                           IN PTEXTMODE_BUFFER_INFO TextModeInfo)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PTEXTMODE_SCREEN_BUFFER NewBuffer = NULL;

    UNREFERENCED_PARAMETER(ProcessHandle);

    if (Console == NULL || Buffer == NULL || TextModeInfo == NULL)
        return STATUS_INVALID_PARAMETER;

    *Buffer = NULL;

    Status = CONSOLE_SCREEN_BUFFER_Initialize((PCONSOLE_SCREEN_BUFFER*)&NewBuffer,
                                              Console,
                                              &TextVtbl,
                                              sizeof(TEXTMODE_SCREEN_BUFFER));
    if (!NT_SUCCESS(Status)) return Status;
    NewBuffer->Header.Type = TEXTMODE_BUFFER;

    NewBuffer->Buffer = ConsoleAllocHeap(HEAP_ZERO_MEMORY,
                                         TextModeInfo->ScreenBufferSize.X *
                                         TextModeInfo->ScreenBufferSize.Y *
                                            sizeof(CHAR_INFO));
    if (NewBuffer->Buffer == NULL)
    {
        CONSOLE_SCREEN_BUFFER_Destroy((PCONSOLE_SCREEN_BUFFER)NewBuffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    NewBuffer->ScreenBufferSize = NewBuffer->OldScreenBufferSize
                                = TextModeInfo->ScreenBufferSize;
    NewBuffer->ViewSize = NewBuffer->OldViewSize
                        = Console->ConsoleSize;

    NewBuffer->ViewOrigin.X = NewBuffer->ViewOrigin.Y = 0;
    NewBuffer->VirtualY = 0;

    NewBuffer->CursorBlinkOn = NewBuffer->ForceCursorOff = FALSE;
    NewBuffer->CursorInfo.bVisible = (TextModeInfo->IsCursorVisible && (TextModeInfo->CursorSize != 0));
    NewBuffer->CursorInfo.dwSize   = min(max(TextModeInfo->CursorSize, 0), 100);

    NewBuffer->ScreenDefaultAttrib = (TextModeInfo->ScreenAttrib & ~COMMON_LVB_SBCSDBCS);
    NewBuffer->PopupDefaultAttrib  = (TextModeInfo->PopupAttrib  & ~COMMON_LVB_SBCSDBCS);

    /* Initialize buffer to be empty with default attributes */
    for (NewBuffer->CursorPosition.Y = 0 ; NewBuffer->CursorPosition.Y < NewBuffer->ScreenBufferSize.Y; NewBuffer->CursorPosition.Y++)
    {
        ClearLineBuffer(NewBuffer);
    }
    NewBuffer->CursorPosition.X = NewBuffer->CursorPosition.Y = 0;

    NewBuffer->Mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;

    *Buffer = (PCONSOLE_SCREEN_BUFFER)NewBuffer;
    return STATUS_SUCCESS;
}

VOID
TEXTMODE_BUFFER_Destroy(IN OUT PCONSOLE_SCREEN_BUFFER Buffer)
{
    PTEXTMODE_SCREEN_BUFFER Buff = (PTEXTMODE_SCREEN_BUFFER)Buffer;

    /*
     * IMPORTANT !! Reinitialize the type so that we don't enter a recursive
     * infinite loop when calling CONSOLE_SCREEN_BUFFER_Destroy.
     */
    Buffer->Header.Type = SCREEN_BUFFER;

    ConsoleFreeHeap(Buff->Buffer);

    CONSOLE_SCREEN_BUFFER_Destroy(Buffer);
}


PCHAR_INFO
ConioCoordToPointer(PTEXTMODE_SCREEN_BUFFER Buff, ULONG X, ULONG Y)
{
    return &Buff->Buffer[((Y + Buff->VirtualY) % Buff->ScreenBufferSize.Y) * Buff->ScreenBufferSize.X + X];
}

/*static*/ VOID
ClearLineBuffer(PTEXTMODE_SCREEN_BUFFER Buff)
{
    PCHAR_INFO Ptr = ConioCoordToPointer(Buff, 0, Buff->CursorPosition.Y);
    SHORT Pos;

    for (Pos = 0; Pos < Buff->ScreenBufferSize.X; Pos++, Ptr++)
    {
        /* Fill the cell */
        Ptr->Char.UnicodeChar = L' ';
        Ptr->Attributes = Buff->ScreenDefaultAttrib;
    }
}

static VOID
ConioComputeUpdateRect(IN PTEXTMODE_SCREEN_BUFFER Buff,
                       IN OUT PSMALL_RECT UpdateRect,
                       IN PCOORD Start,
                       IN UINT Length)
{
    if ((UINT)Buff->ScreenBufferSize.X <= Start->X + Length)
    {
        UpdateRect->Left  = 0;
        UpdateRect->Right = Buff->ScreenBufferSize.X - 1;
    }
    else
    {
        UpdateRect->Left  = Start->X;
        UpdateRect->Right = Start->X + Length - 1;
    }
    UpdateRect->Top = Start->Y;
    UpdateRect->Bottom = Start->Y + (Start->X + Length - 1) / Buff->ScreenBufferSize.X;
    if (Buff->ScreenBufferSize.Y <= UpdateRect->Bottom)
    {
        UpdateRect->Bottom = Buff->ScreenBufferSize.Y - 1;
    }
}

/*
 * Move from one rectangle to another. We must be careful about the order that
 * this is done, to avoid overwriting parts of the source before they are moved.
 */
static VOID
ConioMoveRegion(PTEXTMODE_SCREEN_BUFFER ScreenBuffer,
                PSMALL_RECT SrcRegion,
                PSMALL_RECT DstRegion,
                PSMALL_RECT ClipRegion,
                CHAR_INFO FillChar)
{
    UINT Width  = ConioRectWidth(SrcRegion);
    UINT Height = ConioRectHeight(SrcRegion);
    INT SXOrg, SX, SY;
    INT DXOrg, DX, DY;
    INT XDelta, YDelta;
    UINT i, j;
    CHAR_INFO Cell;
    PCHAR_INFO SRow, DRow;

    SY = SrcRegion->Top;
    DY = DstRegion->Top;
    YDelta = 1;
    if (SY < DY)
    {
        /* Moving down: work from bottom up */
        SY = SrcRegion->Bottom;
        DY = DstRegion->Bottom;
        YDelta = -1;
    }

    SXOrg = SrcRegion->Left;
    DXOrg = DstRegion->Left;
    XDelta = 1;
    if (SXOrg < DXOrg)
    {
        /* Moving right: work from right to left */
        SXOrg = SrcRegion->Right;
        DXOrg = DstRegion->Right;
        XDelta = -1;
    }

    for (i = 0; i < Height; i++)
    {
        SRow = ConioCoordToPointer(ScreenBuffer, 0, SY);
        DRow = ConioCoordToPointer(ScreenBuffer, 0, DY);

        SX = SXOrg;
        DX = DXOrg;

        // TODO: Correctly support "moving" full-width characters.

        for (j = 0; j < Width; j++)
        {
            Cell = SRow[SX];
            if (SX >= ClipRegion->Left && SX <= ClipRegion->Right &&
                SY >= ClipRegion->Top  && SY <= ClipRegion->Bottom)
            {
                SRow[SX] = FillChar;
            }
            if (DX >= ClipRegion->Left && DX <= ClipRegion->Right &&
                DY >= ClipRegion->Top  && DY <= ClipRegion->Bottom)
            {
                DRow[DX] = Cell;
            }
            SX += XDelta;
            DX += XDelta;
        }
        SY += YDelta;
        DY += YDelta;
    }
}

// FIXME!
NTSTATUS NTAPI
ConDrvWriteConsoleInput(IN PCONSOLE Console,
                        IN PCONSOLE_INPUT_BUFFER InputBuffer,
                        IN BOOLEAN AppendToEnd,
                        IN PINPUT_RECORD InputRecord,
                        IN ULONG NumEventsToWrite,
                        OUT PULONG NumEventsWritten OPTIONAL);

NTSTATUS
ConioResizeBuffer(PCONSOLE Console,
                  PTEXTMODE_SCREEN_BUFFER ScreenBuffer,
                  COORD Size)
{
    PCHAR_INFO Buffer;
    PCHAR_INFO Ptr;
    ULONG_PTR Offset = 0;
    WORD CurrentAttribute;
    USHORT CurrentY;
    PCHAR_INFO OldBuffer;
    DWORD i;
    DWORD diff;

    /* Zero size is invalid */
    if (Size.X == 0 || Size.Y == 0)
        return STATUS_INVALID_PARAMETER;

    /* Buffer size is not allowed to be smaller than the view size */
    if (Size.X < ScreenBuffer->ViewSize.X || Size.Y < ScreenBuffer->ViewSize.Y)
        return STATUS_INVALID_PARAMETER;

    if (Size.X == ScreenBuffer->ScreenBufferSize.X && Size.Y == ScreenBuffer->ScreenBufferSize.Y)
    {
        // FIXME: Trigger a buffer resize event ??
        return STATUS_SUCCESS;
    }

    if (Console->FixedSize)
    {
        /*
         * The console is in fixed-size mode, so we cannot resize anything
         * at the moment. However, keep those settings somewhere so that
         * we can try to set them up when we will be allowed to do so.
         */
        ScreenBuffer->OldScreenBufferSize = Size;
        return STATUS_NOT_SUPPORTED; // STATUS_SUCCESS
    }

    Buffer = ConsoleAllocHeap(HEAP_ZERO_MEMORY, Size.X * Size.Y * sizeof(CHAR_INFO));
    if (!Buffer) return STATUS_NO_MEMORY;

    DPRINT("Resizing (%d,%d) to (%d,%d)\n", ScreenBuffer->ScreenBufferSize.X, ScreenBuffer->ScreenBufferSize.Y, Size.X, Size.Y);

    OldBuffer = ScreenBuffer->Buffer;

    for (CurrentY = 0; CurrentY < ScreenBuffer->ScreenBufferSize.Y && CurrentY < Size.Y; CurrentY++)
    {
        Ptr = ConioCoordToPointer(ScreenBuffer, 0, CurrentY);

        if (Size.X <= ScreenBuffer->ScreenBufferSize.X)
        {
            /* Reduce size */
            RtlCopyMemory(Buffer + Offset, Ptr, Size.X * sizeof(CHAR_INFO));
            Offset += Size.X;

            /* If we have cut a trailing full-width character in half, remove it completely */
            Ptr = Buffer + Offset - 1;
            if (Ptr->Attributes & COMMON_LVB_LEADING_BYTE)
            {
                Ptr->Char.UnicodeChar = L' ';
                /* Keep all the other original attributes intact */
                Ptr->Attributes &= ~COMMON_LVB_SBCSDBCS;
            }
        }
        else
        {
            /* Enlarge size */
            RtlCopyMemory(Buffer + Offset, Ptr, ScreenBuffer->ScreenBufferSize.X * sizeof(CHAR_INFO));
            Offset += ScreenBuffer->ScreenBufferSize.X;

            /* The attribute to be used is the one of the last cell of the current line */
            CurrentAttribute = ConioCoordToPointer(ScreenBuffer,
                                                   ScreenBuffer->ScreenBufferSize.X - 1,
                                                   CurrentY)->Attributes;
            CurrentAttribute &= ~COMMON_LVB_SBCSDBCS;

            diff = Size.X - ScreenBuffer->ScreenBufferSize.X;

            /* Zero-out the new part of the buffer */
            for (i = 0; i < diff; i++)
            {
                Ptr = Buffer + Offset;
                Ptr->Char.UnicodeChar = L' ';
                Ptr->Attributes = CurrentAttribute;
                ++Offset;
            }
        }
    }

    if (Size.Y > ScreenBuffer->ScreenBufferSize.Y)
    {
        diff = Size.X * (Size.Y - ScreenBuffer->ScreenBufferSize.Y);

        /* Zero-out the new part of the buffer */
        for (i = 0; i < diff; i++)
        {
            Ptr = Buffer + Offset;
            Ptr->Char.UnicodeChar = L' ';
            Ptr->Attributes = ScreenBuffer->ScreenDefaultAttrib;
            ++Offset;
        }
    }

    (void)InterlockedExchangePointer((PVOID volatile*)&ScreenBuffer->Buffer, Buffer);
    ConsoleFreeHeap(OldBuffer);
    ScreenBuffer->ScreenBufferSize = ScreenBuffer->OldScreenBufferSize = Size;
    ScreenBuffer->VirtualY = 0;

    /* Ensure cursor and window are within buffer */
    if (ScreenBuffer->CursorPosition.X >= Size.X)
        ScreenBuffer->CursorPosition.X = Size.X - 1;
    if (ScreenBuffer->CursorPosition.Y >= Size.Y)
        ScreenBuffer->CursorPosition.Y = Size.Y - 1;
    if (ScreenBuffer->ViewOrigin.X > Size.X - ScreenBuffer->ViewSize.X)
        ScreenBuffer->ViewOrigin.X = Size.X - ScreenBuffer->ViewSize.X;
    if (ScreenBuffer->ViewOrigin.Y > Size.Y - ScreenBuffer->ViewSize.Y)
        ScreenBuffer->ViewOrigin.Y = Size.Y - ScreenBuffer->ViewSize.Y;

    /*
     * Trigger a buffer resize event
     */
    if (Console->InputBuffer.Mode & ENABLE_WINDOW_INPUT)
    {
        ULONG NumEventsWritten;
        INPUT_RECORD er;

        er.EventType = WINDOW_BUFFER_SIZE_EVENT;
        er.Event.WindowBufferSizeEvent.dwSize = ScreenBuffer->ScreenBufferSize;

        // ConioProcessInputEvent(Console, &er);
        ConDrvWriteConsoleInput(Console,
                                &Console->InputBuffer,
                                TRUE,
                                &er,
                                1,
                                &NumEventsWritten);
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvChangeScreenBufferAttributes(IN PCONSOLE Console,
                                   IN PTEXTMODE_SCREEN_BUFFER Buffer,
                                   IN USHORT NewScreenAttrib,
                                   IN USHORT NewPopupAttrib)
{
    USHORT X, Y;
    PCHAR_INFO Ptr;

    COORD  TopLeft = {0};
    ULONG  NumCodesToWrite;
    USHORT OldScreenAttrib, OldPopupAttrib;

    if (Console == NULL || Buffer == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    /* Sanitize the new attributes */
    NewScreenAttrib &= ~COMMON_LVB_SBCSDBCS;
    NewPopupAttrib  &= ~COMMON_LVB_SBCSDBCS;

    NumCodesToWrite = Buffer->ScreenBufferSize.X * Buffer->ScreenBufferSize.Y;
    OldScreenAttrib = Buffer->ScreenDefaultAttrib;
    OldPopupAttrib  = Buffer->PopupDefaultAttrib;

    for (Y = 0; Y < Buffer->ScreenBufferSize.Y; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, 0, Y);
        for (X = 0; X < Buffer->ScreenBufferSize.X; ++X)
        {
            /*
             * Change the current colors only if they are the old ones.
             */

            /* Foreground color */
            if ((Ptr->Attributes & 0x0F) == (OldScreenAttrib & 0x0F))
                Ptr->Attributes = (Ptr->Attributes & 0xFFF0) | (NewScreenAttrib & 0x0F);
            if ((Ptr->Attributes & 0x0F) == (OldPopupAttrib & 0x0F))
                Ptr->Attributes = (Ptr->Attributes & 0xFFF0) | (NewPopupAttrib & 0x0F);

            /* Background color */
            if ((Ptr->Attributes & 0xF0) == (OldScreenAttrib & 0xF0))
                Ptr->Attributes = (Ptr->Attributes & 0xFF0F) | (NewScreenAttrib & 0xF0);
            if ((Ptr->Attributes & 0xF0) == (OldPopupAttrib & 0xF0))
                Ptr->Attributes = (Ptr->Attributes & 0xFF0F) | (NewPopupAttrib & 0xF0);

            ++Ptr;
        }
    }

    /* Save foreground and background attributes for both screen and popup */
    Buffer->ScreenDefaultAttrib = NewScreenAttrib;
    Buffer->PopupDefaultAttrib  = NewPopupAttrib;

    /* Refresh the display if needed */
    if ((PCONSOLE_SCREEN_BUFFER)Buffer == Console->ActiveBuffer)
    {
        SMALL_RECT UpdateRect;
        ConioComputeUpdateRect(Buffer, &UpdateRect, &TopLeft, NumCodesToWrite);
        TermDrawRegion(Console, &UpdateRect);
    }

    return STATUS_SUCCESS;
}


/* PUBLIC DRIVER APIS *********************************************************/

NTSTATUS NTAPI
ConDrvReadConsoleOutput(IN PCONSOLE Console,
                        IN PTEXTMODE_SCREEN_BUFFER Buffer,
                        IN BOOLEAN Unicode,
                        OUT PCHAR_INFO CharInfo/*Buffer*/,
                        IN OUT PSMALL_RECT ReadRegion)
{
    SHORT X, Y;
    SMALL_RECT ScreenBuffer;
    PCHAR_INFO CurCharInfo;
    SMALL_RECT CapturedReadRegion;
    PCHAR_INFO Ptr;

    if (Console == NULL || Buffer == NULL || CharInfo == NULL || ReadRegion == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedReadRegion = *ReadRegion;

    /* Make sure ReadRegion is inside the screen buffer */
    ConioInitRect(&ScreenBuffer, 0, 0,
                  Buffer->ScreenBufferSize.Y - 1,
                  Buffer->ScreenBufferSize.X - 1);
    if (!ConioGetIntersection(&CapturedReadRegion, &CapturedReadRegion, &ScreenBuffer))
    {
        /*
         * It is okay to have a ReadRegion completely outside
         * the screen buffer. No data is read then.
         */
        return STATUS_SUCCESS;
    }

    CurCharInfo = CharInfo;

    for (Y = CapturedReadRegion.Top; Y <= CapturedReadRegion.Bottom; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, CapturedReadRegion.Left, Y);
        for (X = CapturedReadRegion.Left; X <= CapturedReadRegion.Right; ++X)
        {
            if (Unicode)
            {
                CurCharInfo->Char.UnicodeChar = Ptr->Char.UnicodeChar;
            }
            else
            {
                // ConsoleOutputUnicodeToAnsiChar(Console, &CurCharInfo->Char.AsciiChar, &Ptr->Char.UnicodeChar);
                WideCharToMultiByte(Console->OutputCodePage, 0, &Ptr->Char.UnicodeChar, 1,
                                    &CurCharInfo->Char.AsciiChar, 1, NULL, NULL);
            }
#if (_WIN32_WINNT < _WIN32_WINNT_WIN8)
            /* NOTE: Windows < 8 compatibility: DBCS flags are filtered out */
            CurCharInfo->Attributes = (Ptr->Attributes & ~COMMON_LVB_SBCSDBCS);
#else
            CurCharInfo->Attributes = Ptr->Attributes;
#endif
            ++Ptr;
            ++CurCharInfo;
        }
    }

    *ReadRegion = CapturedReadRegion;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvWriteConsoleOutput(IN PCONSOLE Console,
                         IN PTEXTMODE_SCREEN_BUFFER Buffer,
                         IN BOOLEAN Unicode,
                         IN PCHAR_INFO CharInfo/*Buffer*/,
                         IN OUT PSMALL_RECT WriteRegion)
{
    SHORT X, Y;
    SMALL_RECT ScreenBuffer;
    PCHAR_INFO CurCharInfo;
    SMALL_RECT CapturedWriteRegion;
    PCHAR_INFO Ptr;

    if (Console == NULL || Buffer == NULL || CharInfo == NULL || WriteRegion == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedWriteRegion = *WriteRegion;

    /* Make sure WriteRegion is inside the screen buffer */
    ConioInitRect(&ScreenBuffer, 0, 0,
                  Buffer->ScreenBufferSize.Y - 1,
                  Buffer->ScreenBufferSize.X - 1);
    if (!ConioGetIntersection(&CapturedWriteRegion, &CapturedWriteRegion, &ScreenBuffer))
    {
        /*
         * It is okay to have a WriteRegion completely outside
         * the screen buffer. No data is written then.
         */
        return STATUS_SUCCESS;
    }

    CurCharInfo = CharInfo;

    for (Y = CapturedWriteRegion.Top; Y <= CapturedWriteRegion.Bottom; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, CapturedWriteRegion.Left, Y);
        for (X = CapturedWriteRegion.Left; X <= CapturedWriteRegion.Right; ++X)
        {
            if (Unicode)
            {
                Ptr->Char.UnicodeChar = CurCharInfo->Char.UnicodeChar;
            }
            else
            {
                ConsoleOutputAnsiToUnicodeChar(Console, &Ptr->Char.UnicodeChar, &CurCharInfo->Char.AsciiChar);
            }
            // TODO: Sanitize DBCS attributes?
            Ptr->Attributes = CurCharInfo->Attributes;
            ++Ptr;
            ++CurCharInfo;
        }
    }

    TermDrawRegion(Console, &CapturedWriteRegion);

    *WriteRegion = CapturedWriteRegion;

    return STATUS_SUCCESS;
}

/*
 * NOTE: This function is strongly inspired by ConDrvWriteConsoleOutput...
 * FIXME: This function MUST be moved into consrv/conoutput.c because only
 * consrv knows how to manipulate VDM screenbuffers.
 */
NTSTATUS NTAPI
ConDrvWriteConsoleOutputVDM(IN PCONSOLE Console,
                            IN PTEXTMODE_SCREEN_BUFFER Buffer,
                            IN PCHAR_CELL CharInfo/*Buffer*/,
                            IN COORD CharInfoSize,
                            IN PSMALL_RECT WriteRegion)
{
    SHORT X, Y;
    SMALL_RECT ScreenBuffer;
    PCHAR_CELL CurCharInfo;
    SMALL_RECT CapturedWriteRegion;
    PCHAR_INFO Ptr;

    if (Console == NULL || Buffer == NULL || CharInfo == NULL || WriteRegion == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedWriteRegion = *WriteRegion;

    /* Make sure WriteRegion is inside the screen buffer */
    ConioInitRect(&ScreenBuffer, 0, 0,
                  Buffer->ScreenBufferSize.Y - 1,
                  Buffer->ScreenBufferSize.X - 1);
    if (!ConioGetIntersection(&CapturedWriteRegion, &CapturedWriteRegion, &ScreenBuffer))
    {
        /*
         * It is okay to have a WriteRegion completely outside
         * the screen buffer. No data is written then.
         */
        return STATUS_SUCCESS;
    }

    // CurCharInfo = CharInfo;

    for (Y = CapturedWriteRegion.Top; Y <= CapturedWriteRegion.Bottom; ++Y)
    {
        /**/CurCharInfo = CharInfo + Y * CharInfoSize.X + CapturedWriteRegion.Left;/**/

        Ptr = ConioCoordToPointer(Buffer, CapturedWriteRegion.Left, Y);
        for (X = CapturedWriteRegion.Left; X <= CapturedWriteRegion.Right; ++X)
        {
            ConsoleOutputAnsiToUnicodeChar(Console, &Ptr->Char.UnicodeChar, &CurCharInfo->Char);
            Ptr->Attributes = CurCharInfo->Attributes;
            ++Ptr;
            ++CurCharInfo;
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvWriteConsole(IN PCONSOLE Console,
                   IN PTEXTMODE_SCREEN_BUFFER ScreenBuffer,
                   IN BOOLEAN Unicode,
                   IN PVOID StringBuffer,
                   IN ULONG NumCharsToWrite,
                   OUT PULONG NumCharsWritten OPTIONAL)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PWCHAR Buffer = NULL;
    ULONG Written = 0;
    ULONG Length;

    if (Console == NULL || ScreenBuffer == NULL /* || StringBuffer == NULL */)
        return STATUS_INVALID_PARAMETER;

    /* Validity checks */
    ASSERT(Console == ScreenBuffer->Header.Console);
    ASSERT((StringBuffer != NULL) || (StringBuffer == NULL && NumCharsToWrite == 0));

    /* Stop here if the console is paused */
    if (Console->UnpauseEvent != NULL) return STATUS_PENDING;

    /* Convert the string to UNICODE */
    if (Unicode)
    {
        Buffer = StringBuffer;
    }
    else
    {
        Length = MultiByteToWideChar(Console->OutputCodePage, 0,
                                     (PCHAR)StringBuffer,
                                     NumCharsToWrite,
                                     NULL, 0);
        Buffer = ConsoleAllocHeap(0, Length * sizeof(WCHAR));
        if (Buffer)
        {
            MultiByteToWideChar(Console->OutputCodePage, 0,
                                (PCHAR)StringBuffer,
                                NumCharsToWrite,
                                (PWCHAR)Buffer, Length);
        }
        else
        {
            Status = STATUS_NO_MEMORY;
        }
    }

    /* Send it */
    if (Buffer)
    {
        if (NT_SUCCESS(Status))
        {
            Status = TermWriteStream(Console,
                                     ScreenBuffer,
                                     Buffer,
                                     NumCharsToWrite,
                                     TRUE);
            if (NT_SUCCESS(Status))
            {
                Written = NumCharsToWrite;
            }
        }

        if (!Unicode) ConsoleFreeHeap(Buffer);
    }

    if (NumCharsWritten) *NumCharsWritten = Written;

    return Status;
}

static NTSTATUS
IntReadConsoleOutputStringChars(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    OUT PVOID StringBuffer,
    IN BOOLEAN Unicode,
    IN ULONG NumCodesToRead,
    IN PCOORD ReadCoord,
    OUT PULONG NumCodesRead OPTIONAL)
{
    ULONG CodeSize;
    LPBYTE ReadBuffer = StringBuffer;
    SHORT X, Y;
    SHORT XStart = ReadCoord->X;
    ULONG nNumChars = 0;
    PCHAR_INFO Ptr;
    BOOLEAN bCJK = Console->IsCJK;

    CodeSize = (Unicode ? RTL_FIELD_SIZE(CODE_ELEMENT, UnicodeChar)
                        : RTL_FIELD_SIZE(CODE_ELEMENT, AsciiChar));

    for (Y = ReadCoord->Y; Y < Buffer->ScreenBufferSize.Y; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, XStart, Y);
        for (X = XStart; X < Buffer->ScreenBufferSize.X; ++X)
        {
            if (nNumChars >= NumCodesToRead)
                goto Quit;

            /*
             * For Chinese, Japanese and Korean.
             * For full-width characters: copy only the character specified
             * in the leading-byte cell, skipping the trailing-byte cell.
             */
            if (bCJK && (Ptr->Attributes & COMMON_LVB_TRAILING_BYTE))
            {
                /*
                 * Windows "compensates" for the fact this is a full-width
                 * character by reducing the amount of characters to be read.
                 * The understanding being that the specified amount of
                 * characters is also in "units" of (half-width) cells.
                 */
                if (NumCodesToRead > 0) --NumCodesToRead;
                ++Ptr;
                continue;
            }

            if (Unicode)
                *(PWCHAR)ReadBuffer = Ptr->Char.UnicodeChar;
            else
                ConsoleOutputUnicodeToAnsiChar(Console, (PCHAR)ReadBuffer, &Ptr->Char.UnicodeChar);

            ++Ptr;

            ReadBuffer += CodeSize;
            ++nNumChars;
        }
        /* Restart at the beginning of the next line */
        XStart = 0;
    }
Quit:

    if (NumCodesRead)
        *NumCodesRead = nNumChars;

    return STATUS_SUCCESS;
}

static NTSTATUS
IntReadConsoleOutputStringAttributes(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    OUT PWORD StringBuffer,
    IN ULONG NumCodesToRead,
    IN PCOORD ReadCoord,
    OUT PULONG NumCodesRead OPTIONAL)
{
    SHORT X, Y;
    SHORT XStart = ReadCoord->X;
    ULONG nNumChars = 0;
    PCHAR_INFO Ptr;

    for (Y = ReadCoord->Y; Y < Buffer->ScreenBufferSize.Y; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, XStart, Y);
        for (X = XStart; X < Buffer->ScreenBufferSize.X; ++X)
        {
            if (nNumChars >= NumCodesToRead)
                goto Quit;

            *StringBuffer = Ptr->Attributes;
            ++Ptr;

            ++StringBuffer;
            ++nNumChars;
        }
        /* Restart at the beginning of the next line */
        XStart = 0;
    }
Quit:

    if (NumCodesRead)
        *NumCodesRead = nNumChars;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvReadConsoleOutputString(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    IN CODE_TYPE CodeType,
    OUT PVOID StringBuffer,
    IN ULONG NumCodesToRead,
    IN PCOORD ReadCoord,
    OUT PULONG NumCodesRead OPTIONAL)
{
    if (Console == NULL || Buffer == NULL || ReadCoord == NULL /* || EndCoord == NULL */)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity checks */
    ASSERT(Console == Buffer->Header.Console);
    ASSERT((StringBuffer != NULL) || (StringBuffer == NULL && NumCodesToRead == 0));

    if (NumCodesRead)
        *NumCodesRead = 0;

    if (!StringBuffer || (NumCodesToRead == 0))
        return STATUS_SUCCESS; // Nothing to do!

    /* Do nothing if the reading starting point is outside of the screen buffer */
    if ( ReadCoord->X < 0 || ReadCoord->X >= Buffer->ScreenBufferSize.X ||
         ReadCoord->Y < 0 || ReadCoord->Y >= Buffer->ScreenBufferSize.Y )
    {
        return STATUS_SUCCESS;
    }

    NumCodesToRead = min(NumCodesToRead, (ULONG)Buffer->ScreenBufferSize.X * Buffer->ScreenBufferSize.Y);

    switch (CodeType)
    {
        case CODE_ASCII:
        {
            return IntReadConsoleOutputStringChars(Console,
                                                   Buffer,
                                                   StringBuffer,
                                                   FALSE,
                                                   NumCodesToRead,
                                                   ReadCoord,
                                                   NumCodesRead);
        }

        case CODE_UNICODE:
        {
            return IntReadConsoleOutputStringChars(Console,
                                                   Buffer,
                                                   StringBuffer,
                                                   TRUE,
                                                   NumCodesToRead,
                                                   ReadCoord,
                                                   NumCodesRead);
        }

        case CODE_ATTRIBUTE:
        {
            C_ASSERT(RTL_FIELD_SIZE(CODE_ELEMENT, Attribute) == sizeof(WORD));
            return IntReadConsoleOutputStringAttributes(Console,
                                                        Buffer,
                                                        (PWORD)StringBuffer,
                                                        NumCodesToRead,
                                                        ReadCoord,
                                                        NumCodesRead);
        }

        default:
            return STATUS_INVALID_PARAMETER;
    }
}

static NTSTATUS
IntWriteConsoleOutputStringChars(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    IN PVOID StringBuffer,
    IN BOOLEAN Unicode,
    IN ULONG NumCodesToWrite,
    IN PCOORD WriteCoord,
    OUT PULONG NumCodesWritten OPTIONAL)
{
    NTSTATUS Status = STATUS_SUCCESS;
    PWCHAR WriteBuffer = NULL;
    PWCHAR tmpString = NULL;
    ULONG Length;
    SHORT X, Y;
    SHORT XStart = WriteCoord->X;
    ULONG nNumChars = 0;
    PCHAR_INFO Ptr;
    BOOLEAN bCJK = Console->IsCJK;

    /* Convert the string to UNICODE */
    if (Unicode)
    {
        WriteBuffer = StringBuffer;
    }
    else
    {
        /* Convert the ASCII string into Unicode before writing it to the console */
        Length = MultiByteToWideChar(Console->OutputCodePage, 0,
                                     (PCHAR)StringBuffer,
                                     NumCodesToWrite,
                                     NULL, 0);
        tmpString = ConsoleAllocHeap(0, Length * sizeof(WCHAR));
        if (!tmpString)
        {
            Status = STATUS_NO_MEMORY;
            goto Quit;
        }

        MultiByteToWideChar(Console->OutputCodePage, 0,
                            (PCHAR)StringBuffer,
                            NumCodesToWrite,
                            tmpString, Length);

        NumCodesToWrite = Length;
        WriteBuffer = tmpString;
    }

    for (Y = WriteCoord->Y; Y < Buffer->ScreenBufferSize.Y; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, XStart, Y);
        for (X = XStart; X < Buffer->ScreenBufferSize.X; ++X)
        {
            if (nNumChars >= NumCodesToWrite)
                goto Quit;

            /* For Chinese, Japanese and Korean */
            if (bCJK && IS_FULL_WIDTH(*WriteBuffer))
            {
                /* A full-width character cannot cross a line boundary */
                if (X >= Buffer->ScreenBufferSize.X - 1)
                {
                    /* Go to next line */
                    break; // Break the X-loop only.
                }

                /* Set the leading byte */
                Ptr->Char.UnicodeChar = *WriteBuffer;
                Ptr->Attributes &= ~COMMON_LVB_SBCSDBCS;
                Ptr->Attributes |= COMMON_LVB_LEADING_BYTE;
                ++Ptr;

                /* Set the trailing byte */
                Ptr->Char.UnicodeChar = L' ';
                Ptr->Attributes &= ~COMMON_LVB_SBCSDBCS;
                Ptr->Attributes |= COMMON_LVB_TRAILING_BYTE;
            }
            else
            {
                Ptr->Char.UnicodeChar = *WriteBuffer;
            }

            ++Ptr;

            ++WriteBuffer;
            ++nNumChars;
        }
        /* Restart at the beginning of the next line */
        XStart = 0;
    }
Quit:

    if (tmpString)
    {
        ASSERT(!Unicode);
        ConsoleFreeHeap(tmpString);
    }

    if (NumCodesWritten)
        *NumCodesWritten = nNumChars;

    return Status;
}

static NTSTATUS
IntWriteConsoleOutputStringAttribute(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    IN PWORD StringBuffer,
    IN ULONG NumCodesToWrite,
    IN PCOORD WriteCoord,
    OUT PULONG NumCodesWritten OPTIONAL)
{
    SHORT X, Y;
    SHORT XStart = WriteCoord->X;
    ULONG nNumChars = 0;
    PCHAR_INFO Ptr;

    for (Y = WriteCoord->Y; Y < Buffer->ScreenBufferSize.Y; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, XStart, Y);
        for (X = XStart; X < Buffer->ScreenBufferSize.X; ++X)
        {
            if (nNumChars >= NumCodesToWrite)
                goto Quit;

            Ptr->Attributes &= COMMON_LVB_SBCSDBCS;
            Ptr->Attributes |= (*StringBuffer & ~COMMON_LVB_SBCSDBCS);

            ++Ptr;

            ++StringBuffer;
            ++nNumChars;
        }
        /* Restart at the beginning of the next line */
        XStart = 0;
    }
Quit:

    if (NumCodesWritten)
        *NumCodesWritten = nNumChars;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvWriteConsoleOutputString(
    IN PCONSOLE Console,
    IN PTEXTMODE_SCREEN_BUFFER Buffer,
    IN CODE_TYPE CodeType,
    IN PVOID StringBuffer,
    IN ULONG NumCodesToWrite,
    IN PCOORD WriteCoord,
    OUT PULONG NumCodesWritten OPTIONAL)
{
    NTSTATUS Status;

    if (Console == NULL || Buffer == NULL || WriteCoord == NULL /* || EndCoord == NULL */)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity checks */
    ASSERT(Console == Buffer->Header.Console);
    ASSERT((StringBuffer != NULL) || (StringBuffer == NULL && NumCodesToWrite == 0));

    if (NumCodesWritten)
        *NumCodesWritten = 0;

    if (!StringBuffer || (NumCodesToWrite == 0))
        return STATUS_SUCCESS; // Nothing to do!

    /* Do nothing if the writing starting point is outside of the screen buffer */
    if ( WriteCoord->X < 0 || WriteCoord->X >= Buffer->ScreenBufferSize.X ||
         WriteCoord->Y < 0 || WriteCoord->Y >= Buffer->ScreenBufferSize.Y )
    {
        return STATUS_SUCCESS;
    }

    NumCodesToWrite = min(NumCodesToWrite, (ULONG)Buffer->ScreenBufferSize.X * Buffer->ScreenBufferSize.Y);

    switch (CodeType)
    {
        case CODE_ASCII:
        {
            C_ASSERT(RTL_FIELD_SIZE(CODE_ELEMENT, AsciiChar) == sizeof(CHAR));
            Status = IntWriteConsoleOutputStringChars(Console,
                                                      Buffer,
                                                      StringBuffer,
                                                      FALSE,
                                                      NumCodesToWrite,
                                                      WriteCoord,
                                                      NumCodesWritten);
            break;
        }

        case CODE_UNICODE:
        {
            C_ASSERT(RTL_FIELD_SIZE(CODE_ELEMENT, UnicodeChar) == sizeof(WCHAR));
            Status = IntWriteConsoleOutputStringChars(Console,
                                                      Buffer,
                                                      StringBuffer,
                                                      TRUE,
                                                      NumCodesToWrite,
                                                      WriteCoord,
                                                      NumCodesWritten);
            break;
        }

        case CODE_ATTRIBUTE:
        {
            C_ASSERT(RTL_FIELD_SIZE(CODE_ELEMENT, Attribute) == sizeof(WORD));
            Status = IntWriteConsoleOutputStringAttribute(Console,
                                                          Buffer,
                                                          (PWORD)StringBuffer,
                                                          NumCodesToWrite,
                                                          WriteCoord,
                                                          NumCodesWritten);
            break;
        }

        default:
            return STATUS_INVALID_PARAMETER;
    }

    if ((PCONSOLE_SCREEN_BUFFER)Buffer == Console->ActiveBuffer)
    {
        SMALL_RECT UpdateRect;
        ConioComputeUpdateRect(Buffer, &UpdateRect, WriteCoord, NumCodesToWrite);
        TermDrawRegion(Console, &UpdateRect);
    }

    return Status;
}

NTSTATUS NTAPI
ConDrvFillConsoleOutput(IN PCONSOLE Console,
                        IN PTEXTMODE_SCREEN_BUFFER Buffer,
                        IN CODE_TYPE CodeType,
                        IN CODE_ELEMENT Code,
                        IN ULONG NumCodesToWrite,
                        IN PCOORD WriteCoord,
                        OUT PULONG NumCodesWritten OPTIONAL)
{
    SHORT X, Y;
    SHORT XStart;
    ULONG nNumChars = 0;
    PCHAR_INFO Ptr;
    BOOLEAN bLead, bFullwidth;

    if (Console == NULL || Buffer == NULL || WriteCoord == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    if (NumCodesWritten)
        *NumCodesWritten = 0;

    if (NumCodesToWrite == 0)
        return STATUS_SUCCESS; // Nothing to do!

    /* Do nothing if the writing starting point is outside of the screen buffer */
    if ( WriteCoord->X < 0 || WriteCoord->X >= Buffer->ScreenBufferSize.X ||
         WriteCoord->Y < 0 || WriteCoord->Y >= Buffer->ScreenBufferSize.Y )
    {
        return STATUS_SUCCESS;
    }

    NumCodesToWrite = min(NumCodesToWrite, (ULONG)Buffer->ScreenBufferSize.X * Buffer->ScreenBufferSize.Y);

    if (CodeType == CODE_ASCII)
    {
        /* Conversion from the ASCII char to the UNICODE char */
        CODE_ELEMENT tmp;
        ConsoleOutputAnsiToUnicodeChar(Console, &tmp.UnicodeChar, &Code.AsciiChar);
        Code = tmp;
    }

    XStart = WriteCoord->X;

    /* For Chinese, Japanese and Korean */
    X = XStart;
    Y = WriteCoord->Y;
    bLead = TRUE;
    bFullwidth = FALSE;
    if (Console->IsCJK)
    {
        bFullwidth = IS_FULL_WIDTH(Code.UnicodeChar);
        if (X > 0)
        {
            Ptr = ConioCoordToPointer(Buffer, X - 1, Y);
            if (Ptr->Attributes & COMMON_LVB_LEADING_BYTE)
            {
                Ptr->Char.UnicodeChar = L' ';
                Ptr->Attributes &= ~COMMON_LVB_SBCSDBCS;
            }
        }
    }

    for (Y = WriteCoord->Y; Y < Buffer->ScreenBufferSize.Y; ++Y)
    {
        Ptr = ConioCoordToPointer(Buffer, XStart, Y);
        for (X = XStart; X < Buffer->ScreenBufferSize.X; ++X)
        {
            if (nNumChars >= NumCodesToWrite)
                goto Quit;

            switch (CodeType)
            {
                case CODE_ASCII:
                case CODE_UNICODE:
                    Ptr->Char.UnicodeChar = Code.UnicodeChar;
                    Ptr->Attributes &= ~COMMON_LVB_SBCSDBCS;
                    if (bFullwidth)
                    {
                        if (bLead)
                            Ptr->Attributes |= COMMON_LVB_LEADING_BYTE;
                        else
                            Ptr->Attributes |= COMMON_LVB_TRAILING_BYTE;
                    }
                    bLead = !bLead;
                    break;

                case CODE_ATTRIBUTE:
                    Ptr->Attributes &= COMMON_LVB_SBCSDBCS;
                    Ptr->Attributes |= (Code.Attribute & ~COMMON_LVB_SBCSDBCS);
                    break;
            }

            ++Ptr;

            ++nNumChars;
        }
        /* Restart at the beginning of the next line */
        XStart = 0;
    }
Quit:

    if ((nNumChars & 1) & bFullwidth)
    {
        if (X + Y * Buffer->ScreenBufferSize.X > 0)
        {
            Ptr = ConioCoordToPointer(Buffer, X - 1, Y);
            Ptr->Char.UnicodeChar = L' ';
            Ptr->Attributes &= ~COMMON_LVB_SBCSDBCS;
        }
    }

    if (NumCodesWritten)
        *NumCodesWritten = nNumChars;

    if ((PCONSOLE_SCREEN_BUFFER)Buffer == Console->ActiveBuffer)
    {
        SMALL_RECT UpdateRect;
        ConioComputeUpdateRect(Buffer, &UpdateRect, WriteCoord, nNumChars);
        TermDrawRegion(Console, &UpdateRect);
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvGetConsoleScreenBufferInfo(IN  PCONSOLE Console,
                                 IN  PTEXTMODE_SCREEN_BUFFER Buffer,
                                 OUT PCOORD ScreenBufferSize,
                                 OUT PCOORD CursorPosition,
                                 OUT PCOORD ViewOrigin,
                                 OUT PCOORD ViewSize,
                                 OUT PCOORD MaximumViewSize,
                                 OUT PWORD  Attributes)
{
    COORD LargestWindowSize;

    if (Console == NULL || Buffer == NULL || ScreenBufferSize == NULL ||
        CursorPosition  == NULL || ViewOrigin == NULL || ViewSize == NULL ||
        MaximumViewSize == NULL || Attributes == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    *ScreenBufferSize = Buffer->ScreenBufferSize;
    *CursorPosition   = Buffer->CursorPosition;
    *ViewOrigin       = Buffer->ViewOrigin;
    *ViewSize         = Buffer->ViewSize;
    *Attributes       = Buffer->ScreenDefaultAttrib;

    /*
     * Retrieve the largest possible console window size, taking
     * into account the size of the console screen buffer.
     */
    TermGetLargestConsoleWindowSize(Console, &LargestWindowSize);
    LargestWindowSize.X = min(LargestWindowSize.X, Buffer->ScreenBufferSize.X);
    LargestWindowSize.Y = min(LargestWindowSize.Y, Buffer->ScreenBufferSize.Y);
    *MaximumViewSize = LargestWindowSize;

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvSetConsoleTextAttribute(IN PCONSOLE Console,
                              IN PTEXTMODE_SCREEN_BUFFER Buffer,
                              IN WORD Attributes)
{
    if (Console == NULL || Buffer == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    Buffer->ScreenDefaultAttrib = (Attributes & ~COMMON_LVB_SBCSDBCS);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvSetConsoleScreenBufferSize(IN PCONSOLE Console,
                                 IN PTEXTMODE_SCREEN_BUFFER Buffer,
                                 IN PCOORD Size)
{
    NTSTATUS Status;

    if (Console == NULL || Buffer == NULL || Size == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    Status = ConioResizeBuffer(Console, Buffer, *Size);
    if (NT_SUCCESS(Status)) TermResizeTerminal(Console);

    return Status;
}

NTSTATUS NTAPI
ConDrvScrollConsoleScreenBuffer(IN PCONSOLE Console,
                                IN PTEXTMODE_SCREEN_BUFFER Buffer,
                                IN BOOLEAN Unicode,
                                IN PSMALL_RECT ScrollRectangle,
                                IN BOOLEAN UseClipRectangle,
                                IN PSMALL_RECT ClipRectangle OPTIONAL,
                                IN PCOORD DestinationOrigin,
                                IN CHAR_INFO FillChar)
{
    COORD CapturedDestinationOrigin;
    SMALL_RECT ScreenBuffer;
    SMALL_RECT SrcRegion;
    SMALL_RECT DstRegion;
    SMALL_RECT UpdateRegion;
    SMALL_RECT CapturedClipRectangle;

    if (Console == NULL || Buffer == NULL || ScrollRectangle == NULL ||
        (UseClipRectangle ? ClipRectangle == NULL : FALSE) || DestinationOrigin == NULL)
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedDestinationOrigin = *DestinationOrigin;

    /* Make sure the source rectangle is inside the screen buffer */
    ConioInitRect(&ScreenBuffer, 0, 0,
                  Buffer->ScreenBufferSize.Y - 1,
                  Buffer->ScreenBufferSize.X - 1);
    if (!ConioGetIntersection(&SrcRegion, ScrollRectangle, &ScreenBuffer))
    {
        return STATUS_SUCCESS;
    }

    /* If the source was clipped on the left or top, adjust the destination accordingly */
    if (ScrollRectangle->Left < 0)
    {
        CapturedDestinationOrigin.X -= ScrollRectangle->Left;
    }
    if (ScrollRectangle->Top < 0)
    {
        CapturedDestinationOrigin.Y -= ScrollRectangle->Top;
    }

    if (UseClipRectangle)
    {
        CapturedClipRectangle = *ClipRectangle;
        if (!ConioGetIntersection(&CapturedClipRectangle, &CapturedClipRectangle, &ScreenBuffer))
        {
            return STATUS_SUCCESS;
        }
    }
    else
    {
        CapturedClipRectangle = ScreenBuffer;
    }

    ConioInitRect(&DstRegion,
                  CapturedDestinationOrigin.Y,
                  CapturedDestinationOrigin.X,
                  CapturedDestinationOrigin.Y + ConioRectHeight(&SrcRegion) - 1,
                  CapturedDestinationOrigin.X + ConioRectWidth(&SrcRegion ) - 1);

    if (!Unicode)
    {
        WCHAR tmp;
        ConsoleOutputAnsiToUnicodeChar(Console, &tmp, &FillChar.Char.AsciiChar);
        FillChar.Char.UnicodeChar = tmp;
    }
    /* Sanitize the attribute */
    FillChar.Attributes &= ~COMMON_LVB_SBCSDBCS;

    ConioMoveRegion(Buffer, &SrcRegion, &DstRegion, &CapturedClipRectangle, FillChar);

    if ((PCONSOLE_SCREEN_BUFFER)Buffer == Console->ActiveBuffer)
    {
        ConioGetUnion(&UpdateRegion, &SrcRegion, &DstRegion);
        if (ConioGetIntersection(&UpdateRegion, &UpdateRegion, &CapturedClipRectangle))
        {
            /* Draw update region */
            TermDrawRegion(Console, &UpdateRegion);
        }
    }

    return STATUS_SUCCESS;
}

NTSTATUS NTAPI
ConDrvSetConsoleWindowInfo(IN PCONSOLE Console,
                           IN PTEXTMODE_SCREEN_BUFFER Buffer,
                           IN BOOLEAN Absolute,
                           IN PSMALL_RECT WindowRect)
{
    SMALL_RECT CapturedWindowRect;
    COORD LargestWindowSize;

    if (Console == NULL || Buffer == NULL || WindowRect == NULL)
        return STATUS_INVALID_PARAMETER;

    /* Validity check */
    ASSERT(Console == Buffer->Header.Console);

    CapturedWindowRect = *WindowRect;

    if (!Absolute)
    {
        /* Relative positions are given, transform them to absolute ones */
        CapturedWindowRect.Left   += Buffer->ViewOrigin.X;
        CapturedWindowRect.Top    += Buffer->ViewOrigin.Y;
        CapturedWindowRect.Right  += Buffer->ViewOrigin.X + Buffer->ViewSize.X - 1;
        CapturedWindowRect.Bottom += Buffer->ViewOrigin.Y + Buffer->ViewSize.Y - 1;
    }

    /*
     * The MSDN documentation on SetConsoleWindowInfo is partially wrong about
     * the performed checks this API performs. While it is correct that the
     * 'Right'/'Bottom' members cannot be strictly smaller than the 'Left'/'Top'
     * members, they can be equal.
     * Also, if the 'Left' or 'Top' members are negative, this is automatically
     * corrected for, and the window rectangle coordinates are shifted accordingly.
     */
    if ((CapturedWindowRect.Right  < CapturedWindowRect.Left) ||
        (CapturedWindowRect.Bottom < CapturedWindowRect.Top))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /*
     * Forbid window sizes larger than the largest allowed console window size,
     * taking into account the size of the console screen buffer.
     */
    TermGetLargestConsoleWindowSize(Console, &LargestWindowSize);
    LargestWindowSize.X = min(LargestWindowSize.X, Buffer->ScreenBufferSize.X);
    LargestWindowSize.Y = min(LargestWindowSize.Y, Buffer->ScreenBufferSize.Y);
    if ((CapturedWindowRect.Right - CapturedWindowRect.Left + 1 > LargestWindowSize.X) ||
        (CapturedWindowRect.Bottom - CapturedWindowRect.Top + 1 > LargestWindowSize.Y))
    {
        return STATUS_INVALID_PARAMETER;
    }

    /* Shift the window rectangle coordinates if 'Left' or 'Top' are negative */
    if (CapturedWindowRect.Left < 0)
    {
        CapturedWindowRect.Right -= CapturedWindowRect.Left;
        CapturedWindowRect.Left = 0;
    }
    if (CapturedWindowRect.Top < 0)
    {
        CapturedWindowRect.Bottom -= CapturedWindowRect.Top;
        CapturedWindowRect.Top = 0;
    }

    /* Clip the window rectangle to the screen buffer */
    CapturedWindowRect.Right  = min(CapturedWindowRect.Right , Buffer->ScreenBufferSize.X);
    CapturedWindowRect.Bottom = min(CapturedWindowRect.Bottom, Buffer->ScreenBufferSize.Y);

    Buffer->ViewOrigin.X = CapturedWindowRect.Left;
    Buffer->ViewOrigin.Y = CapturedWindowRect.Top;

    Buffer->ViewSize.X = CapturedWindowRect.Right - CapturedWindowRect.Left + 1;
    Buffer->ViewSize.Y = CapturedWindowRect.Bottom - CapturedWindowRect.Top + 1;

    TermResizeTerminal(Console);

    return STATUS_SUCCESS;
}

/* EOF */
