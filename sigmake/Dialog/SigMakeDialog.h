#pragma once

void OpenSigMakeDialog();
void DestroySigMakeDialog();
void ClearSigMakeOutput(HWND hwndDlg);
void WriteSigMakeOutput(HWND hwndDlg, const char * const text);
void WriteSigMakeFormatted(HWND hwndDlg, const char * const format, ...);
