/*
 * Examine - a set of tools for memory leak detection on Windows and
 * PE file reader
 *
 * Copyright (C) 2016 Vincent Torri.
 * All rights reserved.
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
# ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
# endif
# include <windows.h>
# undef WIN32_LEAN_AND_MEAN
# include <wincrypt.h>
# include <wintrust.h>
# include <softpub.h>
#endif

#include <Examine.h>

#include "examine_private.h"

#define ENCODING (X509_ASN_ENCODING | PKCS_7_ASN_ENCODING)
typedef struct
{
    LPWSTR program_name;
    LPWSTR publisher_link;
    LPWSTR info_link;
} Prog_Publisher_Info;

static LPWSTR
_exm_wstrcpy(LPCWSTR str)
{
    LPWSTR res = NULL;

    res = (LPWSTR)malloc((wcslen(str) + 1) * sizeof(WCHAR));
    if (res != NULL)
    {
        lstrcpyW(res, str);
    }
    return res;
}

static wchar_t *
_exm_char_to_wchar(const char *text)
{
    wchar_t *wtext;
    int      wsize;

    if (!text)
        return NULL;

    wsize = MultiByteToWideChar(CP_ACP, 0, text, (int)strlen(text) + 1, NULL, 0);
    if ((wsize == 0) ||
        (wsize > (int)(ULONG_MAX / sizeof(wchar_t))))
    {
        if (wsize == 0)
            printf("error when converting to wchar_t : %ld\n", GetLastError());
        return NULL;
    }

    wtext = malloc(wsize * sizeof(wchar_t));
    if (wtext)
        if (!MultiByteToWideChar(CP_ACP, 0, text, (int)strlen(text) + 1, wtext, wsize))
        {
            EXM_LOG_ERR("error when converting to wchar_t : %ld", GetLastError());
            return NULL;
        }

    return wtext;
}

static void
_exm_sigcheck_signature_get(const Exm_Pe *pe)
{
    WINTRUST_DATA data;
    WINTRUST_FILE_INFO fi;
    LONG res;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    memset(&data, 0, sizeof(data));

    data.cbStruct = sizeof(data);
    data.pPolicyCallbackData = NULL;
    data.pSIPClientData = NULL;
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;

    memset(&fi, 0, sizeof(fi));
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = _exm_char_to_wchar(exm_pe_filename_get(pe));
    if (!fi.pcwszFilePath)
    {
        EXM_LOG_ERR("Can not allocate memory for the application file name");
        return;
    }
    fi.hFile = NULL;
    fi.pgKnownSubject = NULL;

    data.pFile = &fi;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.hWVTStateData = NULL;
    data.pwszURLReference = NULL;
    data.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN;
    data.dwUIContext = WTD_UICONTEXT_EXECUTE;
#if _WIN32_WINNT >=0x0602
    data.pSignatureSettings = NULL;
#endif
    res = WinVerifyTrust(NULL, &action, &data);

    printf("        Signature:          ");
    switch (res)
    {
        case ERROR_SUCCESS:
            printf("signed\n");
            break;
        case TRUST_E_NOSIGNATURE:
        {
            DWORD err = GetLastError();
            if (TRUST_E_NOSIGNATURE == (int)err)
                printf("unsigned\n");
            else if (TRUST_E_PROVIDER_UNKNOWN == (int)err)
                printf("unknown provider\n");
            else
                printf("error\n");
            break;
        }
        case TRUST_E_EXPLICIT_DISTRUST:
            printf("disallowed\n");
            break;
        case TRUST_E_SUBJECT_NOT_TRUSTED:
            printf("not trusted\n");
            break;
        case TRUST_E_SUBJECT_FORM_UNKNOWN:
            printf("unknown file type\n");
            break;
        case TRUST_E_BAD_DIGEST:
            printf("modified or corrupted file\n");
            break;
        case CRYPT_E_SECURITY_SETTINGS:
            printf("user trust disabled\n");
            break;
        case CERT_E_REVOKED:
            printf("revokedd certificate\n");
            break;
        case CERT_E_EXPIRED:
            printf("expired certificate\n");
            break;
        default:
            printf("error\n");
            break;
    }

    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &data);

    return;
}

static BOOL
_exm_sigcheck_program_and_publisher_info_get(const CMSG_SIGNER_INFO *signer_info,
                                             Prog_Publisher_Info *info)
{
    SPC_SP_OPUS_INFO *opus_info;
    DWORD data;
    DWORD i;
    BOOL res = FALSE;

    if (!signer_info)
        return FALSE;
    /*
     * Loop through authenticated attributes and find
     * SPC_SP_OPUS_INFO_OBJID OID.
     */
    for (i = 0; i < signer_info->AuthAttrs.cAttr; i++)
    {
        if (lstrcmpA(SPC_SP_OPUS_INFO_OBJID,
                     signer_info->AuthAttrs.rgAttr[i].pszObjId) != 0)
            continue;

        /* Get the size of SPC_SP_OPUS_INFO structure */
        if (!CryptDecodeObject((X509_ASN_ENCODING | PKCS_7_ASN_ENCODING),
                               SPC_SP_OPUS_INFO_OBJID,
                               signer_info->AuthAttrs.rgAttr[i].rgValue[0].pbData,
                               signer_info->AuthAttrs.rgAttr[i].rgValue[0].cbData,
                               0,
                               NULL,
                               &data))
        {
            EXM_LOG_ERR("CryptDecodeObject() failed (%lx)", GetLastError());
            return FALSE;
        }

        opus_info = (SPC_SP_OPUS_INFO *)calloc(1, data);
        if (!opus_info)
        {
            EXM_LOG_ERR("Can not allocate memory for the Publisher Information");
            return FALSE;
        }

        /* Decode and get SPC_SP_OPUS_INFO structure */
        if (!CryptDecodeObject((X509_ASN_ENCODING | PKCS_7_ASN_ENCODING),
                               SPC_SP_OPUS_INFO_OBJID,
                               signer_info->AuthAttrs.rgAttr[i].rgValue[0].pbData,
                               signer_info->AuthAttrs.rgAttr[i].rgValue[0].cbData,
                               0,
                               opus_info,
                               &data))
        {
            EXM_LOG_ERR("CryptDecodeObject() failed (%lx)", GetLastError());
            free(opus_info);
            return FALSE;
        }

        /* fill program name */
        if (opus_info->pwszProgramName)
            info->program_name =_exm_wstrcpy(opus_info->pwszProgramName);
        else
            info->program_name = NULL;

        /* fill publisher information */
        if (opus_info->pPublisherInfo)
        {
            switch (opus_info->pPublisherInfo->dwLinkChoice)
            {
                case SPC_URL_LINK_CHOICE:
                    info->publisher_link = _exm_wstrcpy(opus_info->pPublisherInfo->pwszUrl);
                    break;
                case SPC_FILE_LINK_CHOICE:
                    info->publisher_link = _exm_wstrcpy(opus_info->pPublisherInfo->pwszFile);
                    break;
                default:
                    info->publisher_link = NULL;
                            break;
            }
        }
        else
            info->publisher_link = NULL;

        /* fill more information */
        if (opus_info->pMoreInfo)
        {
            switch (opus_info->pMoreInfo->dwLinkChoice)
            {
                case SPC_URL_LINK_CHOICE:
                    info->info_link = _exm_wstrcpy(opus_info->pMoreInfo->pwszUrl);
                    break;
                case SPC_FILE_LINK_CHOICE:
                    info->info_link = _exm_wstrcpy(opus_info->pMoreInfo->pwszFile);
                    break;
                default:
                    info->info_link = NULL;
                    break;
            }
        }
        else
            info->info_link = NULL;

        res = TRUE;
        free(opus_info);
        break;
    }

    return res;
}

static BOOL
_exm_sigcheck_timestamp_get(const CMSG_SIGNER_INFO *counter_signer_info,
                            SYSTEMTIME *st)
{
    FILETIME lft;
    FILETIME ft;
    DWORD data;
    DWORD i;
    BOOL ret = FALSE;

    if (!counter_signer_info)
    {
        EXM_LOG_WARN("No unathenticated attributes for szOID_RSA_counterSign OID.");
        return FALSE;
    }

    /*
     * Loop through authenticated attributes and find
     * szOID_RSA_signingTime OID.
     */
    for (i = 0; i < counter_signer_info->AuthAttrs.cAttr; i++)
    {
        if (lstrcmpA(szOID_RSA_signingTime,
                    counter_signer_info->AuthAttrs.rgAttr[i].pszObjId) != 0)
            continue;

        /* Decode and get FILETIME structure. */
        data = sizeof(ft);
        if (!CryptDecodeObject((X509_ASN_ENCODING | PKCS_7_ASN_ENCODING),
                               szOID_RSA_signingTime,
                               counter_signer_info->AuthAttrs.rgAttr[i].rgValue[0].pbData,
                               counter_signer_info->AuthAttrs.rgAttr[i].rgValue[0].cbData,
                               0,
                               (void *)&ft,
                               &data))
        {
            EXM_LOG_ERR("CryptDecodeObject failed with %lx\n",
                        GetLastError());
            return FALSE;
        }

        /* Convert to local time */
        FileTimeToLocalFileTime(&ft, &lft);
        FileTimeToSystemTime(&lft, st);

        ret = TRUE;

        break;
    }

    return ret;
}

static void
_exm_sigcheck_signer_info_get(const char *filename,
                              CMSG_SIGNER_INFO **signer_info,
                              CMSG_SIGNER_INFO **counter_signer_info,
                              const CERT_CONTEXT **signer_cert_context,
                              const CERT_CONTEXT **counter_cert_context)
{
    WCHAR ufilename[MAX_PATH];
    CERT_INFO ci;
    HCERTSTORE store;
    HCRYPTMSG msg;
    DWORD encoding;
    DWORD content;
    DWORD format;
    DWORD size;
    DWORD i;

    *signer_info = NULL;
    *counter_signer_info = NULL;
    *signer_cert_context = NULL;
    *counter_cert_context = NULL;

    if (mbstowcs(ufilename, filename, MAX_PATH) == (size_t)-1)
    {
        EXM_LOG_ERR("Unable to convert %s to unicode", filename);
        return;
    }

    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE,
                          ufilename,
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY,
                          0,
                          &encoding,
                          &content,
                          &format,
                          &store,
                          &msg,
                          NULL))
    {
        EXM_LOG_WARN("CryptQueryObject() failed (%lx)", GetLastError());
        return;
    }

    if (!CryptMsgGetParam(msg,
                          CMSG_SIGNER_INFO_PARAM,
                          0,
                          NULL,
                          &size))
    {
        EXM_LOG_ERR("CryptMsgGetParam() failed (%lx)", GetLastError());
        return;
    }

    *signer_info = (CMSG_SIGNER_INFO *)calloc(1, size);
    if (!*signer_info)
    {
        EXM_LOG_ERR("Can not allocate memory for the Signer Information");
        return;
    }

    if (!CryptMsgGetParam(msg,
                          CMSG_SIGNER_INFO_PARAM,
                          0,
                          (PVOID)*signer_info,
                          &size))
    {
        EXM_LOG_ERR("CryptMsgGetParam() failed (%lx)", GetLastError());
        goto free_signer_info;
    }

    ci.Issuer = (*signer_info)->Issuer;
    ci.SerialNumber = (*signer_info)->SerialNumber;
    *signer_cert_context = CertFindCertificateInStore(store,
                                                      (X509_ASN_ENCODING | PKCS_7_ASN_ENCODING),
                                                      0,
                                                      CERT_FIND_SUBJECT_CERT,
                                                      (void *)&ci,
                                                      NULL);
    if (!*signer_cert_context)
    {
        EXM_LOG_ERR("CertFindCertificateInStore() for signer failed (%lx)", GetLastError());
        goto free_signer_info;
    }

    /* counter_signer_info */

    /*
     * Loop through unathenticated attributes for
     * szOID_RSA_counterSign OID.
     */
    for (i = 0; i < (*signer_info)->UnauthAttrs.cAttr; i++)
    {
        if (lstrcmpA((*signer_info)->UnauthAttrs.rgAttr[i].pszObjId,
                     szOID_RSA_counterSign) != 0)
            continue;

        /* Get size of CMSG_SIGNER_INFO structure. */
        if (!CryptDecodeObject((X509_ASN_ENCODING | PKCS_7_ASN_ENCODING),
                               PKCS7_SIGNER_INFO,
                               (*signer_info)->UnauthAttrs.rgAttr[i].rgValue[0].pbData,
                               (*signer_info)->UnauthAttrs.rgAttr[i].rgValue[0].cbData,
                               0,
                               NULL,
                               &size))
        {
            EXM_LOG_ERR("CryptDecodeObject failed with %lx\n",
                        GetLastError());
            goto free_signer_info;
        }

        /* Allocate memory for CMSG_SIGNER_INFO. */
        *counter_signer_info = (CMSG_SIGNER_INFO *)calloc(1, size);
        if (!*counter_signer_info)
        {
            EXM_LOG_ERR("Can not allocate memory for the Signer Information");
            goto free_signer_info;
        }

        if (!CryptDecodeObject((X509_ASN_ENCODING | PKCS_7_ASN_ENCODING),
                               PKCS7_SIGNER_INFO,
                               (*signer_info)->UnauthAttrs.rgAttr[i].rgValue[0].pbData,
                               (*signer_info)->UnauthAttrs.rgAttr[i].rgValue[0].cbData,
                               0,
                               (void *)*counter_signer_info,
                               &size))
        {
            EXM_LOG_ERR("CryptDecodeObject failed with %lx\n",
                        GetLastError());
            goto free_counter_signer_info;
        }

        break;
    }

    if (*counter_signer_info)
    {
        ci.Issuer = (*counter_signer_info)->Issuer;
        ci.SerialNumber = (*counter_signer_info)->SerialNumber;
        *counter_cert_context = CertFindCertificateInStore(store,
                                                           (X509_ASN_ENCODING | PKCS_7_ASN_ENCODING),
                                                           0,
                                                           CERT_FIND_SUBJECT_CERT,
                                                           (PVOID)&ci,
                                                           NULL);
        if (!*counter_cert_context)
        {
            EXM_LOG_ERR("CertFindCertificateInStore() failed with %lx",
                        GetLastError());
            goto free_counter_signer_info;
        }
    }

    return;

  free_counter_signer_info:
    free(*counter_signer_info);
    *counter_signer_info = NULL;
  free_signer_info:
    free(*signer_info);
    *signer_info = NULL;
    return;
}

static void
_exm_sigcheck_time_disp(const Exm_Pe *pe,
                        CMSG_SIGNER_INFO *counter_signer_info)
{
    SYSTEMTIME st;

    printf("        Signing date:       ");
    if (!_exm_sigcheck_timestamp_get(counter_signer_info, &st))
    {
        FILETIME ft;
        FILETIME lft;
        ULARGE_INTEGER uli;

#define EXM_WINDOWS_TICK 10000000
#define EXM_SEC_TO_UNIX_EPOCH 11644473600ULL
        uli.QuadPart = ((unsigned long long)exm_pe_nt_header_get(pe)->FileHeader.TimeDateStamp + EXM_SEC_TO_UNIX_EPOCH) * EXM_WINDOWS_TICK;
        ft.dwLowDateTime = uli.LowPart;
        ft.dwHighDateTime = uli.HighPart;
        FileTimeToLocalFileTime(&ft, &lft);
        if (!FileTimeToSystemTime(&lft, &st))
        {
            st.wHour = 0;
            st.wMinute = 0;
            st.wDay = 1;
            st.wMonth = 1;
            st.wYear = 1970;
        }
    }

    printf("%02d:%02d %02d/%02d/%04d\n",
           st.wHour,
           st.wMinute,
           st.wDay,
           st.wMonth,
           st.wYear);

    if (counter_signer_info)
        free(counter_signer_info);
}

static void
_exm_sigcheck_certificate_get(CMSG_SIGNER_INFO *signer_info)
{
    Prog_Publisher_Info info;
    BOOL has_info;

    ZeroMemory(&info, sizeof(info));
    has_info = _exm_sigcheck_program_and_publisher_info_get(signer_info,
                                                            &info);
    printf("        Program name:       ");
    wprintf(L"%s\n",
            (has_info && (info.program_name != NULL))
            ? info.program_name
            : L"None");
    printf("        Publisher link:     ");
    wprintf(L"%s\n",
            (has_info && (info.publisher_link != NULL))
            ? info.publisher_link
            : L"None");
    printf("        Information link:   ");
    wprintf(L"%s\n",
            (has_info && (info.info_link != NULL))
            ? info.info_link
            : L"None");

    if (signer_info)
        free(signer_info);
}

static void
_exm_sigcheck_certificate_disp(const CERT_CONTEXT *cert_context)
{
    LPTSTR name;
    DWORD data;

    if (!cert_context)
    {
        printf("          Issuer name:      none\n");
        printf("          subject name:     none\n");
    }
    else
    {
        printf("          Issuer name:      ");
        data = CertGetNameString(cert_context,
                                 CERT_NAME_SIMPLE_DISPLAY_TYPE,
                                 CERT_NAME_ISSUER_FLAG,
                                 NULL,
                                 NULL,
                                 0);
        if (!data)
            printf("none\n");
        else
        {
            name = malloc(data * sizeof(TCHAR));
            if (!name)
                printf("none\n");
            else
            {
                if (!(CertGetNameString(cert_context,
                                        CERT_NAME_SIMPLE_DISPLAY_TYPE,
                                        CERT_NAME_ISSUER_FLAG,
                                        NULL,
                                        name,
                                        data)))
                    printf("none\n");
                else
                    printf("%s\n", name);
                free(name);
            }
        }
        printf("          subject name:     ");

        data = CertGetNameString(cert_context,
                                 CERT_NAME_SIMPLE_DISPLAY_TYPE,
                                 0,
                                 NULL,
                                 NULL,
                                 0);
        if (!data)
            printf("none\n");
        else
        {
            name = malloc(data * sizeof(TCHAR));
            if (!name)
                printf("none\n");
            else
            {
                if (!(CertGetNameString(cert_context,
                                        CERT_NAME_SIMPLE_DISPLAY_TYPE,
                                        0,
                                        NULL,
                                        name,
                                        data)))
                    printf("none\n");
                else
                    printf("%s\n", name);
                free(name);
            }
        }
    }
}

static void
_exm_sigcheck_cmd_run(const Exm_Pe *pe)
{
    CMSG_SIGNER_INFO *signer_info;
    CMSG_SIGNER_INFO *counter_signer_info;
    const CERT_CONTEXT *signer_cert_context;
    const CERT_CONTEXT *counter_cert_context;

    printf("\n%s\n", exm_pe_filename_get(pe));
    _exm_sigcheck_signature_get(pe);
    _exm_sigcheck_signer_info_get(exm_pe_filename_get(pe),
                                  &signer_info,
                                  &counter_signer_info,
                                  &signer_cert_context,
                                  &counter_cert_context);
    _exm_sigcheck_time_disp(pe, counter_signer_info);
    _exm_sigcheck_certificate_get(signer_info);
    printf("        Signer certificate: \n");
    _exm_sigcheck_certificate_disp(signer_cert_context);
    printf("        Counter certificate: \n");
    _exm_sigcheck_certificate_disp(counter_cert_context);
    if (exm_pe_is_64bits(pe) == 1)
        printf("        Machine type:       64-bit\n");
    else if (exm_pe_is_64bits(pe) == 0)
        printf("        Machine type:       32-bit\n");
}

static void
_exm_sigcheck_gui_run(const Exm_Pe *pe, Exm_Log_Level log_level)
{
    EXM_LOG_WARN("GUI not done yet");
    (void)pe;
    (void)log_level;
}

void
exm_sigcheck_run(const char *module, unsigned char gui, Exm_Log_Level log_level)
{
    Exm_Pe *pe;

    pe = exm_pe_new(module);
    if (!pe)
    {
        EXM_LOG_ERR("%s is not a binary nor a DLL.", module);
        return;
    }

#ifdef _WIN32
    if (gui)
        _exm_sigcheck_gui_run(pe, log_level);
    else
#endif
        _exm_sigcheck_cmd_run(pe);

    exm_pe_free(pe);

    EXM_LOG_DBG("resources freed");
}
