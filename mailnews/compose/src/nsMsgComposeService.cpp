/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsMsgComposeService.h"
#include "nsMsgCompCID.h"
#include "nsIMsgSend.h"
#include "nsIServiceManager.h"
#include "nsIObserverService.h"
#include "nsIMsgIdentity.h"
#include "nsISmtpUrl.h"
#include "nsIURI.h"
#include "nsMsgI18N.h"
#include "nsIMsgComposeParams.h"
#include "nsXPCOM.h"
#include "nsISupportsPrimitives.h"
#include "nsIWindowWatcher.h"
#include "mozIDOMWindow.h"
#include "nsIContentViewer.h"
#include "nsIMsgWindow.h"
#include "nsIDocShell.h"
#include "nsPIDOMWindow.h"
#include "nsIDOMDocument.h"
#include "nsIDOMHTMLDocument.h"
#include "nsIDOMElement.h"
#include "nsIXULWindow.h"
#include "nsIWindowMediator.h"
#include "nsIDocShellTreeItem.h"
#include "nsIDocShellTreeOwner.h"
#include "nsIBaseWindow.h"
#include "nsIPrefService.h"
#include "nsIPrefBranch.h"
#include "nsMsgBaseCID.h"
#include "nsIMsgAccountManager.h"
#include "nsIMimeMiscStatus.h"
#include "nsIStreamConverter.h"
#include "nsMsgMimeCID.h"
#include "nsToolkitCompsCID.h"
#include "nsNetUtil.h"
#include "nsIMsgMailNewsUrl.h"
#include "nsIInterfaceRequestorUtils.h"
#include "nsIMsgDatabase.h"
#include "nsIDocumentEncoder.h"
#include "nsContentCID.h"
#include "nsISelection.h"
#include "nsUTF8Utils.h"
#include "nsILineBreaker.h"
#include "nsLWBrkCIID.h"
#include "mozilla/Services.h"
#include "mimemoz2.h"
#include "nsIArray.h"
#include "nsArrayUtils.h"

#ifdef MSGCOMP_TRACE_PERFORMANCE
#include "mozilla/Logging.h"
#include "nsIMsgHdr.h"
#include "nsIMsgMessageService.h"
#include "nsMsgUtils.h"
#endif

#include "nsICommandLine.h"
#include "nsIAppStartup.h"
#include "nsMsgUtils.h"
#include "nsIPrincipal.h"

#ifdef XP_WIN32
#include <windows.h>
#include <shellapi.h>
#include "nsIWidget.h"
#endif

#define DEFAULT_CHROME  "chrome://messenger/content/messengercompose/messengercompose.xul"

#define PREF_MAILNEWS_REPLY_QUOTING_SELECTION            "mailnews.reply_quoting_selection"
#define PREF_MAILNEWS_REPLY_QUOTING_SELECTION_MULTI_WORD "mailnews.reply_quoting_selection.multi_word"
#define PREF_MAILNEWS_REPLY_QUOTING_SELECTION_ONLY_IF    "mailnews.reply_quoting_selection.only_if_chars"

#define MAIL_ROOT_PREF                             "mail."
#define MAILNEWS_ROOT_PREF                         "mailnews."
#define HTMLDOMAINUPDATE_VERSION_PREF_NAME         "global_html_domains.version"
#define HTMLDOMAINUPDATE_DOMAINLIST_PREF_NAME      "global_html_domains"
#define USER_CURRENT_HTMLDOMAINLIST_PREF_NAME      "html_domains"
#define USER_CURRENT_PLAINTEXTDOMAINLIST_PREF_NAME "plaintext_domains"
#define DOMAIN_DELIMITER                           ','

#ifdef MSGCOMP_TRACE_PERFORMANCE
static PRLogModuleInfo *MsgComposeLogModule = nullptr;

static uint32_t GetMessageSizeFromURI(const char * originalMsgURI)
{
  uint32_t msgSize = 0;

  if (originalMsgURI && *originalMsgURI)
  {
    nsCOMPtr <nsIMsgDBHdr> originalMsgHdr;
    GetMsgDBHdrFromURI(originalMsgURI, getter_AddRefs(originalMsgHdr));
    if (originalMsgHdr)
    originalMsgHdr->GetMessageSize(&msgSize);
  }

  return msgSize;
}
#endif

nsMsgComposeService::nsMsgComposeService()
{

// Defaulting the value of mLogComposePerformance to FALSE to prevent logging.
  mLogComposePerformance = false;
#ifdef MSGCOMP_TRACE_PERFORMANCE
  if (!MsgComposeLogModule)
      MsgComposeLogModule = PR_NewLogModule("msgcompose");

  mStartTime = PR_IntervalNow();
  mPreviousTime = mStartTime;
#endif

}

NS_IMPL_ISUPPORTS(nsMsgComposeService,
                   nsIMsgComposeService,
                   ICOMMANDLINEHANDLER,
                   nsISupportsWeakReference)

nsMsgComposeService::~nsMsgComposeService()
{
  mOpenComposeWindows.Clear();
}

nsresult nsMsgComposeService::Init()
{
  nsresult rv = NS_OK;

  Reset();

  AddGlobalHtmlDomains();
  // Since the compose service should only be initialized once, we can
  // be pretty sure there aren't any existing compose windows open.
  MsgCleanupTempFiles("nsmail", "tmp");
  MsgCleanupTempFiles("nsemail", "html");
  MsgCleanupTempFiles("nscopy", "tmp");
  return rv;
}

void nsMsgComposeService::Reset()
{
  mOpenComposeWindows.Clear();

  nsCOMPtr<nsIPrefBranch> prefs(do_GetService(NS_PREFSERVICE_CONTRACTID));
  if (prefs)
    prefs->GetBoolPref("mailnews.logComposePerformance", &mLogComposePerformance);
}

// Function to open a message compose window and pass an nsIMsgComposeParams
// parameter to it.
NS_IMETHODIMP
nsMsgComposeService::OpenComposeWindowWithParams(const char *chrome,
                                                 nsIMsgComposeParams *params)
{
  NS_ENSURE_ARG_POINTER(params);
#ifdef MSGCOMP_TRACE_PERFORMANCE
  if(mLogComposePerformance)
  {
    TimeStamp("Start opening the window", true);
  }
#endif

  nsresult rv;

  NS_ENSURE_ARG_POINTER(params);

  //Use default identity if no identity has been specified
  nsCOMPtr<nsIMsgIdentity> identity;
  params->GetIdentity(getter_AddRefs(identity));
  if (!identity)
  {
    GetDefaultIdentity(getter_AddRefs(identity));
    params->SetIdentity(identity);
  }

  // Create a new window.
  nsCOMPtr<nsIWindowWatcher> wwatch(do_GetService(NS_WINDOWWATCHER_CONTRACTID));
  if (!wwatch)
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsISupportsInterfacePointer> msgParamsWrapper =
    do_CreateInstance(NS_SUPPORTS_INTERFACE_POINTER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  msgParamsWrapper->SetData(params);
  msgParamsWrapper->SetDataIID(&NS_GET_IID(nsIMsgComposeParams));

  nsCOMPtr<mozIDOMWindowProxy> newWindow;
  rv = wwatch->OpenWindow(0, chrome && *chrome ? chrome : DEFAULT_CHROME,
                 "_blank", "all,chrome,dialog=no,status,toolbar", msgParamsWrapper,
                 getter_AddRefs(newWindow));

  return rv;
}

NS_IMETHODIMP
nsMsgComposeService::DetermineComposeHTML(nsIMsgIdentity *aIdentity, MSG_ComposeFormat aFormat, bool *aComposeHTML)
{
  NS_ENSURE_ARG_POINTER(aComposeHTML);

  *aComposeHTML = true;
  switch (aFormat)
  {
    case nsIMsgCompFormat::HTML:
      *aComposeHTML = true;
      break;
    case nsIMsgCompFormat::PlainText:
      *aComposeHTML = false;
      break;

    default:
      nsCOMPtr<nsIMsgIdentity> identity = aIdentity;
      if (!identity)
        GetDefaultIdentity(getter_AddRefs(identity));

      if (identity)
      {
        identity->GetComposeHtml(aComposeHTML);
        if (aFormat == nsIMsgCompFormat::OppositeOfDefault)
          *aComposeHTML = !*aComposeHTML;
      }
      else
      {
        // default identity not found.  Use the mail.html_compose pref to determine
        // message compose type (HTML or PlainText).
        nsCOMPtr<nsIPrefBranch> prefs(do_GetService(NS_PREFSERVICE_CONTRACTID));
        if (prefs)
        {
          nsresult rv;
          bool useHTMLCompose;
          rv = prefs->GetBoolPref(MAIL_ROOT_PREF "html_compose", &useHTMLCompose);
          if (NS_SUCCEEDED(rv))
            *aComposeHTML = useHTMLCompose;
        }
      }
      break;
  }

  return NS_OK;
}

nsresult
nsMsgComposeService::GetOrigWindowSelection(MSG_ComposeType type, nsIMsgWindow *aMsgWindow, nsACString& aSelHTML)
{
  nsresult rv;

  // Good hygiene
  aSelHTML.Truncate();

  // Get the pref to see if we even should do reply quoting selection
  nsCOMPtr<nsIPrefBranch> prefs(do_GetService(NS_PREFSERVICE_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  bool replyQuotingSelection;
  rv = prefs->GetBoolPref(PREF_MAILNEWS_REPLY_QUOTING_SELECTION, &replyQuotingSelection);
  NS_ENSURE_SUCCESS(rv, rv);
  if (!replyQuotingSelection)
    return NS_ERROR_ABORT;

  // Now delve down in to the message to get the HTML representation of the selection
  nsCOMPtr<nsIDocShell> rootDocShell;
  rv = aMsgWindow->GetRootDocShell(getter_AddRefs(rootDocShell));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIDocShellTreeItem> childAsItem;
  rv = rootDocShell->FindChildWithName(NS_LITERAL_STRING("messagepane"),
                                       true, false, nullptr, nullptr, getter_AddRefs(childAsItem));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIDocShell> docShell(do_QueryInterface(childAsItem, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<mozIDOMWindowProxy> domWindow(do_GetInterface(childAsItem));
  NS_ENSURE_TRUE(domWindow, NS_ERROR_FAILURE);
  nsCOMPtr<nsPIDOMWindowOuter> privateWindow = nsPIDOMWindowOuter::From(domWindow);
  nsCOMPtr<nsISelection> sel = privateWindow->GetSelection();
  NS_ENSURE_TRUE(sel, NS_ERROR_FAILURE);

  bool requireMultipleWords = true;
  nsAutoCString charsOnlyIf;
  prefs->GetBoolPref(PREF_MAILNEWS_REPLY_QUOTING_SELECTION_MULTI_WORD, &requireMultipleWords);
  prefs->GetCharPref(PREF_MAILNEWS_REPLY_QUOTING_SELECTION_ONLY_IF, getter_Copies(charsOnlyIf));
  if (sel && (requireMultipleWords || !charsOnlyIf.IsEmpty()))
  {
    nsAutoString selPlain;
    rv = sel->ToString(selPlain);
    NS_ENSURE_SUCCESS(rv, rv);

    // If "mailnews.reply_quoting_selection.multi_word" is on, then there must be at least
    // two words selected in order to quote just the selected text
    if (requireMultipleWords)
    {
      if (selPlain.IsEmpty())
        return NS_ERROR_ABORT;

      nsCOMPtr<nsILineBreaker> lineBreaker = do_GetService(NS_LBRK_CONTRACTID, &rv);

      if (NS_SUCCEEDED(rv))
      {
        const uint32_t length = selPlain.Length();
        const char16_t* unicodeStr = selPlain.get();
        int32_t endWordPos = lineBreaker->Next(unicodeStr, length, 0);

        // If there's not even one word, then there's not multiple words
        if (endWordPos == NS_LINEBREAKER_NEED_MORE_TEXT)
          return NS_ERROR_ABORT;

        // If after the first word is only space, then there's not multiple words
        const char16_t* end;
        for (end = unicodeStr + endWordPos; NS_IsSpace(*end); end++)
          ;
        if (!*end)
          return NS_ERROR_ABORT;
      }
    }

    if (!charsOnlyIf.IsEmpty())
    {
      if (MsgFindCharInSet(selPlain, charsOnlyIf.get()) < 0)
        return NS_ERROR_ABORT;
    }
  }

  nsCOMPtr<nsIContentViewer> contentViewer;
  rv = docShell->GetContentViewer(getter_AddRefs(contentViewer));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIDOMDocument> domDocument;
  rv = contentViewer->GetDOMDocument(getter_AddRefs(domDocument));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIDocumentEncoder> docEncoder(do_CreateInstance(NS_HTMLCOPY_ENCODER_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  rv = docEncoder->Init(domDocument, NS_LITERAL_STRING("text/html"), 0);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = docEncoder->SetSelection(sel);
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoString selHTML;
  rv = docEncoder->EncodeToString(selHTML);
  NS_ENSURE_SUCCESS(rv, rv);

  // Now remove <span class="moz-txt-citetags">&gt; </span>.
  nsAutoCString html(NS_ConvertUTF16toUTF8(selHTML).get());
  int32_t spanInd = html.Find("<span class=\"moz-txt-citetags\">");
  while (spanInd != kNotFound) {
    nsAutoCString right0(Substring(html, spanInd));
    int32_t endInd = right0.Find("</span>");
    if (endInd == kNotFound)
      break;  // oops, where is the closing tag gone?
    nsAutoCString right1(Substring(html, spanInd + endInd + 7));
    html.SetLength(spanInd);
    html.Append(right1);
    spanInd = html.Find("<span class=\"moz-txt-citetags\">");
  }

  aSelHTML.Assign(html);

  return rv;
}

NS_IMETHODIMP
nsMsgComposeService::OpenComposeWindow(const char *msgComposeWindowURL, nsIMsgDBHdr *origMsgHdr, const char *originalMsgURI,
  MSG_ComposeType type, MSG_ComposeFormat format, nsIMsgIdentity * aIdentity, nsIMsgWindow *aMsgWindow)
{
  nsresult rv;

  // Check for any reply type that wants to ignore the quote.
  bool ignoreQuote = false;
  if (type >= nsIMsgCompType::ReplyIgnoreQuote) {
    type -= nsIMsgCompType::ReplyIgnoreQuote;
    ignoreQuote = true;
  }

  nsCOMPtr<nsIMsgIdentity> identity = aIdentity;
  if (!identity)
    GetDefaultIdentity(getter_AddRefs(identity));

  /* Actually, the only way to implement forward inline is to simulate a template message.
     Maybe one day when we will have more time we can change that
  */
  if (type == nsIMsgCompType::ForwardInline || type == nsIMsgCompType::Draft ||
      type == nsIMsgCompType::Template || 
      type == nsIMsgCompType::ReplyWithTemplate || 
      type == nsIMsgCompType::Redirect ||
      type == nsIMsgCompType::EditAsNew)
  {
    nsAutoCString uriToOpen(originalMsgURI);
    uriToOpen += (uriToOpen.FindChar('?') == kNotFound) ? '?' : '&';
    uriToOpen.Append("fetchCompleteMessage=true");
    // The compose type that gets transmitted to a compose window open in mime
    // is communicated using url query parameters here.

    if (type == nsIMsgCompType::Redirect)
      uriToOpen.Append("&redirect=true");
    else if (type == nsIMsgCompType::EditAsNew)
      uriToOpen.Append("&editasnew=true");

    return LoadDraftOrTemplate(uriToOpen, type == nsIMsgCompType::ForwardInline || type == nsIMsgCompType::Draft ?
                               nsMimeOutput::nsMimeMessageDraftOrTemplate : nsMimeOutput::nsMimeMessageEditorTemplate,
                               identity, originalMsgURI, origMsgHdr, type == nsIMsgCompType::ForwardInline,
                               format == nsIMsgCompFormat::OppositeOfDefault, aMsgWindow);
  }

  nsCOMPtr<nsIMsgComposeParams> pMsgComposeParams (do_CreateInstance(NS_MSGCOMPOSEPARAMS_CONTRACTID, &rv));
  if (NS_SUCCEEDED(rv) && pMsgComposeParams)
  {
    nsCOMPtr<nsIMsgCompFields> pMsgCompFields (do_CreateInstance(NS_MSGCOMPFIELDS_CONTRACTID, &rv));
    if (NS_SUCCEEDED(rv) && pMsgCompFields)
    {
      pMsgComposeParams->SetType(type);
      pMsgComposeParams->SetFormat(format);
      pMsgComposeParams->SetIdentity(identity);

      // When doing a reply (except with a template) see if there's a selection that we should quote
      if (!ignoreQuote &&
          (type == nsIMsgCompType::Reply ||
           type == nsIMsgCompType::ReplyAll ||
           type == nsIMsgCompType::ReplyToSender ||
           type == nsIMsgCompType::ReplyToGroup ||
           type == nsIMsgCompType::ReplyToSenderAndGroup ||
           type == nsIMsgCompType::ReplyToList))
      {
        nsAutoCString selHTML;
        if (NS_SUCCEEDED(GetOrigWindowSelection(type, aMsgWindow, selHTML)))
          pMsgComposeParams->SetHtmlToQuote(selHTML);
      }

      if (originalMsgURI && *originalMsgURI)
      {
        if (type == nsIMsgCompType::NewsPost)
        {
          nsAutoCString newsURI(originalMsgURI);
          nsAutoCString group;
          nsAutoCString host;

          int32_t slashpos = newsURI.RFindChar('/');
          if (slashpos > 0 )
          {
            // uri is "[s]news://host[:port]/group"
            host = StringHead(newsURI, slashpos);
            group = Substring(newsURI, slashpos + 1);

          }
          else
            group = originalMsgURI;

          nsAutoCString unescapedName;
          MsgUnescapeString(group,
                            nsINetUtil::ESCAPE_URL_FILE_BASENAME | nsINetUtil::ESCAPE_URL_FORCED,
                            unescapedName);
          pMsgCompFields->SetNewsgroups(NS_ConvertUTF8toUTF16(unescapedName));
          pMsgCompFields->SetNewspostUrl(host.get());
        }
        else
        {
          pMsgComposeParams->SetOriginalMsgURI(originalMsgURI);
          pMsgComposeParams->SetOrigMsgHdr(origMsgHdr);
        }
      }

      pMsgComposeParams->SetComposeFields(pMsgCompFields);

      if(mLogComposePerformance)
      {
#ifdef MSGCOMP_TRACE_PERFORMANCE
        // ducarroz, properly fix this in the case of new message (not a reply)
        if (type != nsIMsgCompType::NewsPost) {
          char buff[256];
          sprintf(buff, "Start opening the window, message size = %d", GetMessageSizeFromURI(originalMsgURI));
          TimeStamp(buff, true);
        }
#endif
      }//end if(mLogComposePerformance)

      rv = OpenComposeWindowWithParams(msgComposeWindowURL, pMsgComposeParams);
    }
  }
  return rv;
}

NS_IMETHODIMP nsMsgComposeService::GetParamsForMailto(nsIURI * aURI, nsIMsgComposeParams ** aParams)
{
  nsresult rv = NS_OK;
  if (aURI)
  {
    nsCOMPtr<nsIMailtoUrl> aMailtoUrl;
    rv = aURI->QueryInterface(NS_GET_IID(nsIMailtoUrl), getter_AddRefs(aMailtoUrl));
    if (NS_SUCCEEDED(rv))
    {
       MSG_ComposeFormat requestedComposeFormat = nsIMsgCompFormat::Default;
       nsCString toPart;
       nsCString ccPart;
       nsCString bccPart;
       nsCString subjectPart;
       nsCString bodyPart;
       nsCString newsgroup;
       nsCString refPart;
       nsCString HTMLBodyPart;

       aMailtoUrl->GetMessageContents(toPart, ccPart, bccPart, subjectPart,
                                      bodyPart, HTMLBodyPart, refPart,
                                      newsgroup, &requestedComposeFormat);

      nsAutoString sanitizedBody;

      bool composeHTMLFormat;
      DetermineComposeHTML(NULL, requestedComposeFormat, &composeHTMLFormat);

      // If there was an 'html-body' param, finding it will have requested
      // HTML format in GetMessageContents, so we try to use it first. If it's
      // empty, but we are composing in HTML because of the user's prefs, the
      // 'body' param needs to be escaped, since it's supposed to be plain
      // text, but it then doesn't need to sanitized.
      nsString rawBody;
      if (HTMLBodyPart.IsEmpty())
      {
        if (composeHTMLFormat)
        {
          char *escaped = MsgEscapeHTML(bodyPart.get());
          if (!escaped)
            return NS_ERROR_OUT_OF_MEMORY;

          CopyUTF8toUTF16(nsDependentCString(escaped), sanitizedBody);
          free(escaped);
        }
        else
          CopyUTF8toUTF16(bodyPart, rawBody);
      }
      else
        CopyUTF8toUTF16(HTMLBodyPart, rawBody);

      if (!rawBody.IsEmpty() && composeHTMLFormat)
      {
        //For security reason, we must sanitize the message body before accepting any html...

        rv = HTMLSanitize(rawBody, sanitizedBody); // from mimemoz2.h

        if (NS_FAILED(rv))
        {
          // Something went horribly wrong with parsing for html format
          // in the body.  Set composeHTMLFormat to false so we show the
          // plain text mail compose.
          composeHTMLFormat = false;
        }
      }

      nsCOMPtr<nsIMsgComposeParams> pMsgComposeParams (do_CreateInstance(NS_MSGCOMPOSEPARAMS_CONTRACTID, &rv));
      if (NS_SUCCEEDED(rv) && pMsgComposeParams)
      {
        pMsgComposeParams->SetType(nsIMsgCompType::MailToUrl);
        pMsgComposeParams->SetFormat(composeHTMLFormat ? nsIMsgCompFormat::HTML : nsIMsgCompFormat::PlainText);


        nsCOMPtr<nsIMsgCompFields> pMsgCompFields (do_CreateInstance(NS_MSGCOMPFIELDS_CONTRACTID, &rv));
        if (pMsgCompFields)
        {
          //ugghh more conversion work!!!!
          pMsgCompFields->SetTo(NS_ConvertUTF8toUTF16(toPart));
          pMsgCompFields->SetCc(NS_ConvertUTF8toUTF16(ccPart));
          pMsgCompFields->SetBcc(NS_ConvertUTF8toUTF16(bccPart));
          pMsgCompFields->SetNewsgroups(NS_ConvertUTF8toUTF16(newsgroup));
          pMsgCompFields->SetReferences(refPart.get());
          pMsgCompFields->SetSubject(NS_ConvertUTF8toUTF16(subjectPart));
          pMsgCompFields->SetBody(composeHTMLFormat ? sanitizedBody : rawBody);
          pMsgComposeParams->SetComposeFields(pMsgCompFields);

          NS_ADDREF(*aParams = pMsgComposeParams);
          return NS_OK;
        }
      } // if we created msg compose params....
    } // if we had a mailto url
  } // if we had a url...

  // if we got here we must have encountered an error
  *aParams = nullptr;
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP nsMsgComposeService::OpenComposeWindowWithURI(const char * aMsgComposeWindowURL, nsIURI * aURI, nsIMsgIdentity *identity)
{
  nsCOMPtr<nsIMsgComposeParams> pMsgComposeParams;
  nsresult rv = GetParamsForMailto(aURI, getter_AddRefs(pMsgComposeParams));
  if (NS_SUCCEEDED(rv)) {
    pMsgComposeParams->SetIdentity(identity);
    rv = OpenComposeWindowWithParams(aMsgComposeWindowURL, pMsgComposeParams);
  }
  return rv;
}

NS_IMETHODIMP nsMsgComposeService::InitCompose(nsIMsgComposeParams *aParams,
                                               mozIDOMWindowProxy *aWindow,
                                               nsIDocShell *aDocShell,
                                               nsIMsgCompose **_retval)
{
  nsresult rv;
  nsCOMPtr<nsIMsgCompose> msgCompose =
    do_CreateInstance(NS_MSGCOMPOSE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv,rv);

  rv = msgCompose->Initialize(aParams, aWindow, aDocShell);
  NS_ENSURE_SUCCESS(rv,rv);

  NS_IF_ADDREF(*_retval = msgCompose);
  return rv;
}

NS_IMETHODIMP
nsMsgComposeService::GetDefaultIdentity(nsIMsgIdentity **_retval)
{
  NS_ENSURE_ARG_POINTER(_retval);
  *_retval = nullptr;

  nsresult rv;
  nsCOMPtr<nsIMsgAccountManager> accountManager = do_GetService(NS_MSGACCOUNTMANAGER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgAccount> defaultAccount;
  rv = accountManager->GetDefaultAccount(getter_AddRefs(defaultAccount));
  NS_ENSURE_SUCCESS(rv, rv);

  return defaultAccount ? defaultAccount->GetDefaultIdentity(_retval) : NS_OK;
}

/* readonly attribute boolean logComposePerformance; */
NS_IMETHODIMP nsMsgComposeService::GetLogComposePerformance(bool *aLogComposePerformance)
{
  *aLogComposePerformance = mLogComposePerformance;
  return NS_OK;
}

NS_IMETHODIMP nsMsgComposeService::TimeStamp(const char * label, bool resetTime)
{
  if (!mLogComposePerformance)
    return NS_OK;

#ifdef MSGCOMP_TRACE_PERFORMANCE

  PRIntervalTime now;

  if (resetTime)
  {
    MOZ_LOG(MsgComposeLogModule, mozilla::LogLevel::Info, ("\n[process]: [totalTime][deltaTime]\n--------------------\n"));

    mStartTime = PR_IntervalNow();
    mPreviousTime = mStartTime;
    now = mStartTime;
  }
  else
    now = PR_IntervalNow();

  PRIntervalTime totalTime = PR_IntervalToMilliseconds(now - mStartTime);
  PRIntervalTime deltaTime = PR_IntervalToMilliseconds(now - mPreviousTime);

  MOZ_LOG(MsgComposeLogModule, mozilla::LogLevel::Info, ("[%3.2f][%3.2f] - %s\n",
((double)totalTime/1000.0) + 0.005, ((double)deltaTime/1000.0) + 0.005, label));

  mPreviousTime = now;
#endif
  return NS_OK;
}

class nsMsgTemplateReplyHelper final: public nsIStreamListener,
                                      public nsIUrlListener
{
public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIURLLISTENER
  NS_DECL_NSISTREAMLISTENER
  NS_DECL_NSIREQUESTOBSERVER

  nsMsgTemplateReplyHelper();

  nsCOMPtr<nsIMsgDBHdr> mHdrToReplyTo;
  nsCOMPtr<nsIMsgDBHdr> mTemplateHdr;
  nsCOMPtr<nsIMsgWindow> mMsgWindow;
  nsCOMPtr<nsIMsgIdentity> mIdentity;
  nsCString mTemplateBody;
  bool mInMsgBody;
  char mLastBlockChars[3];

private:
  ~nsMsgTemplateReplyHelper();
};

NS_IMPL_ISUPPORTS(nsMsgTemplateReplyHelper,
                   nsIStreamListener,
                   nsIRequestObserver,
                   nsIUrlListener)

nsMsgTemplateReplyHelper::nsMsgTemplateReplyHelper()
{
  mInMsgBody = false;
  memset(mLastBlockChars, 0, sizeof(mLastBlockChars));
}

nsMsgTemplateReplyHelper::~nsMsgTemplateReplyHelper()
{
}


NS_IMETHODIMP nsMsgTemplateReplyHelper::OnStartRunningUrl(nsIURI *aUrl)
{
  return NS_OK;
}

NS_IMETHODIMP nsMsgTemplateReplyHelper::OnStopRunningUrl(nsIURI *aUrl, nsresult aExitCode)
{
  NS_ENSURE_SUCCESS(aExitCode, aExitCode);
  nsresult rv;
  nsCOMPtr<nsPIDOMWindowOuter> parentWindow;
  if (mMsgWindow)
  {
    nsCOMPtr<nsIDocShell> docShell;
    rv = mMsgWindow->GetRootDocShell(getter_AddRefs(docShell));
    NS_ENSURE_SUCCESS(rv, rv);
    parentWindow = do_GetInterface(docShell);
    NS_ENSURE_TRUE(parentWindow, NS_ERROR_FAILURE);
  }

  // create the compose params object
  nsCOMPtr<nsIMsgComposeParams> pMsgComposeParams (do_CreateInstance(NS_MSGCOMPOSEPARAMS_CONTRACTID, &rv));
  if (NS_FAILED(rv) || (!pMsgComposeParams) ) return rv ;
  nsCOMPtr<nsIMsgCompFields> compFields = do_CreateInstance(NS_MSGCOMPFIELDS_CONTRACTID, &rv) ;

  nsCString replyTo;
  mHdrToReplyTo->GetStringProperty("replyTo", getter_Copies(replyTo));
  if (replyTo.IsEmpty())
    mHdrToReplyTo->GetAuthor(getter_Copies(replyTo));
  compFields->SetTo(NS_ConvertUTF8toUTF16(replyTo));

  nsString body;
  nsString templateSubject, replySubject;

  mHdrToReplyTo->GetMime2DecodedSubject(replySubject);
  mTemplateHdr->GetMime2DecodedSubject(templateSubject);
  nsString subject(NS_LITERAL_STRING("Auto: ")); // RFC 3834 3.1.5.
  subject.Append(templateSubject);
  if (!replySubject.IsEmpty())
  {
    subject.Append(NS_LITERAL_STRING(" (was: "));
    subject.Append(replySubject);
    subject.Append(NS_LITERAL_STRING(")"));
  }

  compFields->SetSubject(subject);
  compFields->SetRawHeader("Auto-Submitted", NS_LITERAL_CSTRING("auto-replied"), nullptr);

  nsCString charset;
  rv = mTemplateHdr->GetCharset(getter_Copies(charset));
  NS_ENSURE_SUCCESS(rv, rv);
  compFields->SetCharacterSet(charset.get());
  rv = nsMsgI18NConvertToUnicode(charset.get(), mTemplateBody, body);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rv), "couldn't convert templ body to unicode");
  compFields->SetBody(body);

  nsCString msgUri;
  nsCOMPtr <nsIMsgFolder> folder;
  mHdrToReplyTo->GetFolder(getter_AddRefs(folder));
  folder->GetUriForMsg(mHdrToReplyTo, msgUri);
  // populate the compose params
  pMsgComposeParams->SetType(nsIMsgCompType::ReplyWithTemplate);
  pMsgComposeParams->SetFormat(nsIMsgCompFormat::Default);
  pMsgComposeParams->SetIdentity(mIdentity);
  pMsgComposeParams->SetComposeFields(compFields);
  pMsgComposeParams->SetOriginalMsgURI(msgUri.get());

  // create the nsIMsgCompose object to send the object
  nsCOMPtr<nsIMsgCompose> pMsgCompose (do_CreateInstance(NS_MSGCOMPOSE_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  /** initialize nsIMsgCompose, Send the message, wait for send completion response **/

  rv = pMsgCompose->Initialize(pMsgComposeParams, parentWindow, nullptr);
  NS_ENSURE_SUCCESS(rv,rv);

  return pMsgCompose->SendMsg(nsIMsgSend::nsMsgDeliverNow, mIdentity, nullptr, nullptr, nullptr) ;
}

NS_IMETHODIMP
nsMsgTemplateReplyHelper::OnStartRequest(nsIRequest* request, nsISupports* aSupport)
{
  return NS_OK;
}

NS_IMETHODIMP
nsMsgTemplateReplyHelper::OnStopRequest(nsIRequest* request, nsISupports* aSupport,
                                nsresult status)
{
  if (NS_SUCCEEDED(status))
  {
    // now we've got the message body in mTemplateBody -
    // need to set body in compose params and send the reply.
  }
  return NS_OK;
}

NS_IMETHODIMP
nsMsgTemplateReplyHelper::OnDataAvailable(nsIRequest* request,
                                  nsISupports* aSupport,
                                  nsIInputStream* inStream,
                                  uint64_t srcOffset,
                                  uint32_t count)
{
  nsresult rv = NS_OK;

  char readBuf[1024];

  uint64_t available;
  uint32_t readCount;
  uint32_t maxReadCount = sizeof(readBuf) - 1;

  rv = inStream->Available(&available);
  while (NS_SUCCEEDED(rv) && available > 0)
  {
    uint32_t bodyOffset = 0, readOffset = 0;
    if (!mInMsgBody && mLastBlockChars[0])
    {
      memcpy(readBuf, mLastBlockChars, 3);
      readOffset = 3;
      maxReadCount -= 3;
    }
    if (maxReadCount > available)
      maxReadCount = (uint32_t)available;
    memset(readBuf, 0, sizeof(readBuf));
    rv = inStream->Read(readBuf + readOffset, maxReadCount, &readCount);
    available -= readCount;
    readCount += readOffset;
    // we're mainly interested in the msg body, so we need to
    // find the header/body delimiter of a blank line. A blank line
    // looks like <CR><CR>, <LF><LF>, or <CRLF><CRLF>
    if (!mInMsgBody)
    {
      for (uint32_t charIndex = 0; charIndex < readCount && !bodyOffset; charIndex++)
      {
        if (readBuf[charIndex] == '\r' || readBuf[charIndex] == '\n')
        {
          if (charIndex + 1 < readCount)
          {
            if (readBuf[charIndex] == readBuf[charIndex + 1])
            {
            // got header+body separator
              bodyOffset = charIndex + 2;
              break;
            }
            else if ((charIndex + 3 < readCount) && !strncmp(readBuf + charIndex, "\r\n\r\n", 4))
            {
              bodyOffset = charIndex + 4;
              break;
            }
          }
        }
      }
      mInMsgBody = bodyOffset != 0;
      if (!mInMsgBody && readCount > 3) // still in msg hdrs
        strncpy(mLastBlockChars, readBuf + readCount - 3, 3);
    }
    mTemplateBody.Append(readBuf + bodyOffset);
  }
  return NS_OK;
}


NS_IMETHODIMP nsMsgComposeService::ReplyWithTemplate(nsIMsgDBHdr *aMsgHdr, const char *templateUri,
                                             nsIMsgWindow *aMsgWindow, nsIMsgIncomingServer *aServer)
{
  // To reply with template, we need the message body of the template.
  // I think we're going to need to stream the template message to ourselves,
  // and construct the body, and call setBody on the compFields.
  nsresult rv;
  nsCOMPtr <nsIMsgAccountManager> accountManager =
    do_GetService(NS_MSGACCOUNTMANAGER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgAccount> account;
  rv = accountManager->FindAccountForServer(aServer, getter_AddRefs(account));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIArray> identities;
  rv = account->GetIdentities(getter_AddRefs(identities));
  NS_ENSURE_SUCCESS(rv, rv);

  nsAutoCString recipients;
  aMsgHdr->GetRecipients(getter_Copies(recipients));

  nsAutoCString ccList;
  aMsgHdr->GetCcList(getter_Copies(ccList));

  // Go through the identities to see to whom this was addressed.
  // In case we get no match, this is likely a list/bulk/bcc/spam mail and we
  // shouldn't reply. RFC 3834 2.
  nsCOMPtr<nsIMsgIdentity> identity; // identity to reply from
  uint32_t count = 0;
  identities->GetLength(&count);
  for (uint32_t i = 0; i < count; i++)
  {
    nsCOMPtr<nsIMsgIdentity> anIdentity(do_QueryElementAt(identities, i, &rv));
    NS_ENSURE_SUCCESS(rv, rv);

    nsAutoCString identityEmail;
    anIdentity->GetEmail(identityEmail);

    if (recipients.Find(identityEmail, CaseInsensitiveCompare) != kNotFound ||
        ccList.Find(identityEmail, CaseInsensitiveCompare) != kNotFound)
    {
      identity = anIdentity;
      break;
    }
  }
  if (!identity) // Found no match -> don't reply.
    return NS_ERROR_ABORT;

  RefPtr<nsMsgTemplateReplyHelper> helper = new nsMsgTemplateReplyHelper;

  helper->mHdrToReplyTo = aMsgHdr;
  helper->mMsgWindow = aMsgWindow;
  helper->mIdentity = identity;

  nsAutoCString replyTo;
  aMsgHdr->GetStringProperty("replyTo", getter_Copies(replyTo));
  if (replyTo.IsEmpty())
    aMsgHdr->GetAuthor(getter_Copies(replyTo));
  if (replyTo.IsEmpty())
    return NS_ERROR_FAILURE; // nowhere to send the reply

  nsCOMPtr <nsIMsgFolder> templateFolder;
  nsCOMPtr <nsIMsgDatabase> templateDB;
  nsCString templateMsgHdrUri;
  const char * query = PL_strstr(templateUri, "?messageId=");
  if (!query)
    return NS_ERROR_FAILURE;

  nsAutoCString folderUri(Substring(templateUri, query));
  rv = GetExistingFolder(folderUri, getter_AddRefs(templateFolder));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = templateFolder->GetMsgDatabase(getter_AddRefs(templateDB));
  NS_ENSURE_SUCCESS(rv, rv);

  const char *subject = PL_strstr(templateUri, "&subject=");
  if (subject)
  {
    const char *subjectEnd = subject + strlen(subject);
    nsAutoCString messageId(Substring(query + 11, subject));
    nsAutoCString subjectString(Substring(subject + 9, subjectEnd));
    templateDB->GetMsgHdrForMessageID(messageId.get(), getter_AddRefs(helper->mTemplateHdr));
    if (helper->mTemplateHdr)
      templateFolder->GetUriForMsg(helper->mTemplateHdr, templateMsgHdrUri);
    // to use the subject, we'd need to expose a method to find a message by subject,
    // or painfully iterate through messages...We'll try to make the message-id
    // not change when saving a template first.
  }
  if (templateMsgHdrUri.IsEmpty())
  {
    // ### probably want to return a specific error and
    // have the calling code disable the filter.
    NS_ASSERTION(false, "failed to get msg hdr");
    return NS_ERROR_FAILURE;
  }
  // we need to convert the template uri, which is of the form
  // <folder uri>?messageId=<messageId>&subject=<subject>
  nsCOMPtr <nsIMsgMessageService> msgService;
  rv = GetMessageServiceFromURI(templateMsgHdrUri, getter_AddRefs(msgService));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsISupports> listenerSupports;
  helper->QueryInterface(NS_GET_IID(nsISupports), getter_AddRefs(listenerSupports));

  nsCOMPtr<nsIURI> dummyNull;
  rv = msgService->StreamMessage(templateMsgHdrUri.get(), listenerSupports,
                                 aMsgWindow, helper,
                                 false, // convert data
                                 EmptyCString(), false, getter_AddRefs(dummyNull));
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgFolder> folder;
  aMsgHdr->GetFolder(getter_AddRefs(folder));
  if (!folder)
    return NS_ERROR_NULL_POINTER;

  // We're sending a new message. Conceptually it's a reply though, so mark the
  // original message as replied.
  return folder->AddMessageDispositionState(aMsgHdr, nsIMsgFolder::nsMsgDispositionState_Replied);
}

NS_IMETHODIMP
nsMsgComposeService::ForwardMessage(const nsAString &forwardTo,
                                    nsIMsgDBHdr *aMsgHdr,
                                    nsIMsgWindow *aMsgWindow,
                                    nsIMsgIncomingServer *aServer,
                                    uint32_t aForwardType)
{
  NS_ENSURE_ARG_POINTER(aMsgHdr);

  nsresult rv;
  if (aForwardType == nsIMsgComposeService::kForwardAsDefault)
  {
    int32_t forwardPref = 0;
    nsCOMPtr<nsIPrefBranch> prefBranch(do_GetService(NS_PREFSERVICE_CONTRACTID, &rv));
    NS_ENSURE_SUCCESS(rv, rv);
    prefBranch->GetIntPref("mail.forward_message_mode", &forwardPref);
    // 0=default as attachment 2=forward as inline with attachments,
    // (obsolete 4.x value)1=forward as quoted (mapped to 2 in mozilla)
    aForwardType = forwardPref == 0 ? nsIMsgComposeService::kForwardAsAttachment :
                                      nsIMsgComposeService::kForwardInline;
  }
  nsCString msgUri;

  nsCOMPtr<nsIMsgFolder> folder;
  aMsgHdr->GetFolder(getter_AddRefs(folder));
  NS_ENSURE_TRUE(folder, NS_ERROR_NULL_POINTER);

  folder->GetUriForMsg(aMsgHdr, msgUri);

  nsAutoCString uriToOpen(msgUri);
  uriToOpen += (uriToOpen.FindChar('?') == kNotFound) ? '?' : '&';
  uriToOpen.Append("fetchCompleteMessage=true");

  // get the MsgIdentity for the above key using AccountManager
  nsCOMPtr<nsIMsgAccountManager> accountManager =
    do_GetService (NS_MSGACCOUNTMANAGER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIMsgAccount> account;
  nsCOMPtr<nsIMsgIdentity> identity;

  rv = accountManager->FindAccountForServer(aServer, getter_AddRefs(account));
  NS_ENSURE_SUCCESS(rv, rv);
  rv = account->GetDefaultIdentity(getter_AddRefs(identity));
  // Use default identity if no identity has been found on this account
  if (NS_FAILED(rv) || !identity)
  {
    rv = GetDefaultIdentity(getter_AddRefs(identity));
    NS_ENSURE_SUCCESS(rv, rv);
  }

  if (aForwardType == nsIMsgComposeService::kForwardInline)
    return RunMessageThroughMimeDraft(uriToOpen,
                                      nsMimeOutput::nsMimeMessageDraftOrTemplate,
                                      identity,
                                      uriToOpen.get(), aMsgHdr,
                                      true, forwardTo,
                                      false, aMsgWindow);

  nsCOMPtr<mozIDOMWindowProxy> parentWindow;
  if (aMsgWindow)
  {
    nsCOMPtr<nsIDocShell> docShell;
    rv = aMsgWindow->GetRootDocShell(getter_AddRefs(docShell));
    NS_ENSURE_SUCCESS(rv, rv);
    parentWindow = do_GetInterface(docShell);
    NS_ENSURE_TRUE(parentWindow, NS_ERROR_FAILURE);
  }
  // create the compose params object
  nsCOMPtr<nsIMsgComposeParams> pMsgComposeParams (do_CreateInstance(NS_MSGCOMPOSEPARAMS_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);
  nsCOMPtr<nsIMsgCompFields> compFields = do_CreateInstance(NS_MSGCOMPFIELDS_CONTRACTID, &rv);

  compFields->SetTo(forwardTo);
  // populate the compose params
  pMsgComposeParams->SetType(nsIMsgCompType::ForwardAsAttachment);
  pMsgComposeParams->SetFormat(nsIMsgCompFormat::Default);
  pMsgComposeParams->SetIdentity(identity);
  pMsgComposeParams->SetComposeFields(compFields);
  pMsgComposeParams->SetOriginalMsgURI(uriToOpen.get());
  // create the nsIMsgCompose object to send the object
  nsCOMPtr<nsIMsgCompose> pMsgCompose (do_CreateInstance(NS_MSGCOMPOSE_CONTRACTID, &rv));
  NS_ENSURE_SUCCESS(rv, rv);

  /** initialize nsIMsgCompose, Send the message, wait for send completion response **/
  rv = pMsgCompose->Initialize(pMsgComposeParams, parentWindow, nullptr);
  NS_ENSURE_SUCCESS(rv, rv);

  rv = pMsgCompose->SendMsg(nsIMsgSend::nsMsgDeliverNow, identity, nullptr, nullptr, nullptr);
  NS_ENSURE_SUCCESS(rv, rv);

  // nsMsgCompose::ProcessReplyFlags usually takes care of marking messages
  // as forwarded. ProcessReplyFlags is normally called from
  // nsMsgComposeSendListener::OnStopSending but for this case the msgCompose
  // object is not set so ProcessReplyFlags won't get called.
  // Therefore, let's just mark it here instead.
  return folder->AddMessageDispositionState(aMsgHdr, nsIMsgFolder::nsMsgDispositionState_Forwarded);
}

nsresult nsMsgComposeService::AddGlobalHtmlDomains()
{

  nsresult rv;
  nsCOMPtr<nsIPrefService> prefs = do_GetService(NS_PREFSERVICE_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv,rv);

  nsCOMPtr<nsIPrefBranch> prefBranch;
  rv = prefs->GetBranch(MAILNEWS_ROOT_PREF, getter_AddRefs(prefBranch));
  NS_ENSURE_SUCCESS(rv,rv);

  nsCOMPtr<nsIPrefBranch> defaultsPrefBranch;
  rv = prefs->GetDefaultBranch(MAILNEWS_ROOT_PREF, getter_AddRefs(defaultsPrefBranch));
  NS_ENSURE_SUCCESS(rv,rv);

  /**
   * Check to see if we need to add any global domains.
   * If so, make sure the following prefs are added to mailnews.js
   *
   * 1. pref("mailnews.global_html_domains.version", version number);
   * This pref registers the current version in the user prefs file. A default value is stored
   * in mailnews file. Depending the changes we plan to make we can move the default version number.
   * Comparing version number from user's prefs file and the default one from mailnews.js, we
   * can effect ppropriate changes.
   *
   * 2. pref("mailnews.global_html_domains", <comma separated domain list>);
   * This pref contains the list of html domains that ISP can add to make that user's contain all
   * of these under the HTML domains in the Mail&NewsGrpus|Send Format under global preferences.
   */
  int32_t htmlDomainListCurrentVersion, htmlDomainListDefaultVersion;
  rv = prefBranch->GetIntPref(HTMLDOMAINUPDATE_VERSION_PREF_NAME, &htmlDomainListCurrentVersion);
  NS_ENSURE_SUCCESS(rv,rv);

  rv = defaultsPrefBranch->GetIntPref(HTMLDOMAINUPDATE_VERSION_PREF_NAME, &htmlDomainListDefaultVersion);
  NS_ENSURE_SUCCESS(rv,rv);

  // Update the list as needed
  if (htmlDomainListCurrentVersion <= htmlDomainListDefaultVersion) {
    // Get list of global domains need to be added
    nsCString globalHtmlDomainList;
    rv = prefBranch->GetCharPref(HTMLDOMAINUPDATE_DOMAINLIST_PREF_NAME, getter_Copies(globalHtmlDomainList));

    if (NS_SUCCEEDED(rv) && !globalHtmlDomainList.IsEmpty()) {
      nsTArray<nsCString> domainArray;

      // Get user's current HTML domain set for send format
      nsCString currentHtmlDomainList;
      rv = prefBranch->GetCharPref(USER_CURRENT_HTMLDOMAINLIST_PREF_NAME, getter_Copies(currentHtmlDomainList));
      NS_ENSURE_SUCCESS(rv,rv);

      nsAutoCString newHtmlDomainList(currentHtmlDomainList);
      // Get the current html domain list into new list var
      ParseString(currentHtmlDomainList, DOMAIN_DELIMITER, domainArray);

      // Get user's current Plaintext domain set for send format
      nsCString currentPlaintextDomainList;
      rv = prefBranch->GetCharPref(USER_CURRENT_PLAINTEXTDOMAINLIST_PREF_NAME, getter_Copies(currentPlaintextDomainList));
      NS_ENSURE_SUCCESS(rv,rv);

      // Get the current plaintext domain list into new list var
      ParseString(currentPlaintextDomainList, DOMAIN_DELIMITER, domainArray);

      size_t i = domainArray.Length();
      if (i > 0) {
        // Append each domain in the preconfigured html domain list
        globalHtmlDomainList.StripWhitespace();
        ParseString(globalHtmlDomainList, DOMAIN_DELIMITER, domainArray);

        // Now add each domain that does not already appear in
        // the user's current html or plaintext domain lists
        for (; i < domainArray.Length(); i++) {
          if (domainArray.IndexOf(domainArray[i]) == i) {
            if (!newHtmlDomainList.IsEmpty())
              newHtmlDomainList += DOMAIN_DELIMITER;
            newHtmlDomainList += domainArray[i];
          }
        }
      }
      else
      {
        // User has no domains listed either in html or plain text category.
        // Assign the global list to be the user's current html domain list
        newHtmlDomainList = globalHtmlDomainList;
      }

      // Set user's html domain pref with the updated list
      rv = prefBranch->SetCharPref(USER_CURRENT_HTMLDOMAINLIST_PREF_NAME, newHtmlDomainList.get());
      NS_ENSURE_SUCCESS(rv,rv);

      // Increase the version to avoid running the update code unless needed (based on default version)
      rv = prefBranch->SetIntPref(HTMLDOMAINUPDATE_VERSION_PREF_NAME, htmlDomainListCurrentVersion + 1);
      NS_ENSURE_SUCCESS(rv,rv);
    }
  }
  return NS_OK;
}

NS_IMETHODIMP
nsMsgComposeService::RegisterComposeDocShell(nsIDocShell *aDocShell,
                                             nsIMsgCompose *aComposeObject)
{
  NS_ENSURE_ARG_POINTER(aDocShell);
  NS_ENSURE_ARG_POINTER(aComposeObject);

  nsresult rv;

  // add the msg compose / dom window mapping to our hash table
  nsCOMPtr<nsIWeakReference> weakDocShell = do_GetWeakReference(aDocShell, &rv);
  NS_ENSURE_SUCCESS(rv,rv);
  nsCOMPtr<nsIWeakReference> weakMsgComposePtr = do_GetWeakReference(aComposeObject);
  NS_ENSURE_SUCCESS(rv,rv);
  mOpenComposeWindows.Put(weakDocShell, weakMsgComposePtr);

  return rv;
}

NS_IMETHODIMP
nsMsgComposeService::UnregisterComposeDocShell(nsIDocShell *aDocShell)
{
  NS_ENSURE_ARG_POINTER(aDocShell);

  nsresult rv;
  nsCOMPtr<nsIWeakReference> weakDocShell = do_GetWeakReference(aDocShell, &rv);
  NS_ENSURE_SUCCESS(rv,rv);

  mOpenComposeWindows.Remove(weakDocShell);

  return rv;
}

NS_IMETHODIMP
nsMsgComposeService::GetMsgComposeForDocShell(nsIDocShell *aDocShell,
                                              nsIMsgCompose **aComposeObject)
{
  NS_ENSURE_ARG_POINTER(aDocShell);
  NS_ENSURE_ARG_POINTER(aComposeObject);

  if (!mOpenComposeWindows.Count())
    return NS_ERROR_FAILURE;

  // get the weak reference for our dom window
  nsresult rv;
  nsCOMPtr<nsIWeakReference> weakDocShell = do_GetWeakReference(aDocShell, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  nsCOMPtr<nsIWeakReference> weakMsgComposePtr;

  if (!mOpenComposeWindows.Get(weakDocShell,
                               getter_AddRefs(weakMsgComposePtr)))
    return NS_ERROR_FAILURE;

  nsCOMPtr<nsIMsgCompose> msgCompose = do_QueryReferent(weakMsgComposePtr, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  NS_IF_ADDREF(*aComposeObject = msgCompose);
  return rv;
}

/**
 * LoadDraftOrTemplate
 *   Helper routine used to run msgURI through libmime in order to fetch the contents for a
 *   draft or template.
 */
nsresult
nsMsgComposeService::LoadDraftOrTemplate(const nsACString& aMsgURI, nsMimeOutputType aOutType,
                                         nsIMsgIdentity * aIdentity, const char * aOriginalMsgURI,
                                         nsIMsgDBHdr * aOrigMsgHdr,
                                         bool aForwardInline,
                                         bool overrideComposeFormat,
                                         nsIMsgWindow *aMsgWindow)
{
  return RunMessageThroughMimeDraft(aMsgURI, aOutType, aIdentity,
                                    aOriginalMsgURI, aOrigMsgHdr,
                                    aForwardInline, EmptyString(),
                                    overrideComposeFormat, aMsgWindow);
}

/**
 * Run the aMsgURI message through libmime. We set various attributes of the
 * nsIMimeStreamConverter so mimedrft.cpp will know what to do with the message
 * when its done streaming. Usually that will be opening a compose window
 * with the contents of the message, but if forwardTo is non-empty, mimedrft.cpp
 * will forward the contents directly.
 *
 * @param aMsgURI URI to stream, which is the msgUri + any extra terms, e.g.,
 *                "redirect=true".
 * @param aOutType  nsMimeOutput::nsMimeMessageDraftOrTemplate or
 *                  nsMimeOutput::nsMimeMessageEditorTemplate
 * @param aIdentity identity to use for the new message
 * @param aOriginalMsgURI msgURI w/o any extra terms
 * @param aOrigMsgHdr nsIMsgDBHdr corresponding to aOriginalMsgURI
 * @param aForwardInline true if doing a forward inline
 * @param aForwardTo  e-mail address to forward msg to. This is used for
 *                     forward inline message filter actions.
 * @param aOverrideComposeFormat True if the user had shift key down when
                                 doing a command that opens the compose window,
 *                               which means we switch the compose window used
 *                               from the default.
 * @param aMsgWindow msgWindow to pass into DisplayMessage.
 */
nsresult
nsMsgComposeService::RunMessageThroughMimeDraft(
            const nsACString& aMsgURI, nsMimeOutputType aOutType,
            nsIMsgIdentity * aIdentity, const char * aOriginalMsgURI,
            nsIMsgDBHdr * aOrigMsgHdr,
            bool aForwardInline,
            const nsAString &aForwardTo,
            bool aOverrideComposeFormat,
            nsIMsgWindow *aMsgWindow)
{
  nsCOMPtr <nsIMsgMessageService> messageService;
  nsresult rv = GetMessageServiceFromURI(aMsgURI, getter_AddRefs(messageService));
  NS_ENSURE_SUCCESS(rv, rv);

  // Create a mime parser (nsIMimeStreamConverter)to do the conversion.
  nsCOMPtr<nsIMimeStreamConverter> mimeConverter =
    do_CreateInstance(NS_MAILNEWS_MIME_STREAM_CONVERTER_CONTRACTID, &rv);
  NS_ENSURE_SUCCESS(rv, rv);

  mimeConverter->SetMimeOutputType(aOutType); // Set the type of output for libmime
  mimeConverter->SetForwardInline(aForwardInline);
  if (!aForwardTo.IsEmpty())
  {
    mimeConverter->SetForwardInlineFilter(true);
    mimeConverter->SetForwardToAddress(aForwardTo);
  }
  mimeConverter->SetOverrideComposeFormat(aOverrideComposeFormat);
  mimeConverter->SetIdentity(aIdentity);
  mimeConverter->SetOriginalMsgURI(aOriginalMsgURI);
  mimeConverter->SetOrigMsgHdr(aOrigMsgHdr);

  nsCOMPtr<nsIURI> url;
  bool fileUrl = StringBeginsWith(aMsgURI, NS_LITERAL_CSTRING("file:"));
  nsCString mailboxUri(aMsgURI);
  if (fileUrl)
  {
    // We loaded a .eml file from a file: url. Construct equivalent mailbox url.
    mailboxUri.Replace(0, 5, NS_LITERAL_CSTRING("mailbox:"));
    mailboxUri.Append(NS_LITERAL_CSTRING("&number=0"));
    // Need this to prevent nsMsgCompose::TagEmbeddedObjects from setting
    // inline images as moz-do-not-send.
    mimeConverter->SetOriginalMsgURI(mailboxUri.get());
  }
  if (fileUrl || PromiseFlatCString(aMsgURI).Find("&type=application/x-message-display") >= 0)
    rv = NS_NewURI(getter_AddRefs(url), mailboxUri);
  else
    rv = messageService->GetUrlForUri(PromiseFlatCString(aMsgURI).get(), getter_AddRefs(url), aMsgWindow);
  NS_ENSURE_SUCCESS(rv, rv);

  // ignore errors here - it's not fatal, and in the case of mailbox messages,
  // we're always passing in an invalid spec...
  (void )url->SetSpec(mailboxUri);

  // if we are forwarding a message and that message used a charset over ride
  // then use that over ride charset instead of the charset specified in the message
  nsCString mailCharset;
  if (aMsgWindow)
  {
    bool charsetOverride;
    if (NS_SUCCEEDED(aMsgWindow->GetCharsetOverride(&charsetOverride)) && charsetOverride)
    {
      if (NS_SUCCEEDED(aMsgWindow->GetMailCharacterSet(mailCharset)))
      {
        nsCOMPtr<nsIMsgI18NUrl> i18nUrl(do_QueryInterface(url));
        if (i18nUrl)
          (void) i18nUrl->SetCharsetOverRide(mailCharset.get());
      }
    }
  }

  nsCOMPtr<nsIPrincipal> nullPrincipal =
    do_CreateInstance("@mozilla.org/nullprincipal;1", &rv);
  NS_ASSERTION(NS_SUCCEEDED(rv), "CreateInstance of nullprincipal failed");
  if (NS_FAILED(rv))
    return rv;

  nsCOMPtr<nsIChannel> channel;
  rv = NS_NewInputStreamChannel(getter_AddRefs(channel),
                     url,
                     nullptr,
                     nullPrincipal,
                     nsILoadInfo::SEC_ALLOW_CROSS_ORIGIN_DATA_IS_NULL,
                     nsIContentPolicy::TYPE_OTHER);
  NS_ASSERTION(NS_SUCCEEDED(rv), "NS_NewChannel failed.");
  if (NS_FAILED(rv))
    return rv;

  nsCOMPtr<nsIStreamConverter> converter = do_QueryInterface(mimeConverter);
  rv = converter->AsyncConvertData(nullptr, nullptr, nullptr, channel);
  NS_ENSURE_SUCCESS(rv, rv);

  // Now, just plug the two together and get the hell out of the way!
  nsCOMPtr<nsIStreamListener> streamListener = do_QueryInterface(mimeConverter);
  nsCOMPtr<nsIURI> dummyNull;
  return messageService->DisplayMessage(PromiseFlatCString(aMsgURI).get(), streamListener,
                                        aMsgWindow, nullptr, mailCharset.get(),
                                        getter_AddRefs(dummyNull));
}

NS_IMETHODIMP
nsMsgComposeService::Handle(nsICommandLine* aCmdLine)
{
  NS_ENSURE_ARG_POINTER(aCmdLine);

  nsresult rv;
  int32_t found, end, count;
  nsAutoString uristr;
  bool composeShouldHandle = true;

  rv = aCmdLine->FindFlag(NS_LITERAL_STRING("compose"), false, &found);
  NS_ENSURE_SUCCESS(rv, rv);

  // MAC OS X passes in -url mailto:mscott@mozilla.org into the command line
  // instead of -compose.
  if (found == -1)
  {
    rv = aCmdLine->FindFlag(NS_LITERAL_STRING("url"), false, &found);
    // we don't want to consume the argument for -url unless we're sure it is a mailto url and we'll
    // figure that out shortly.
    composeShouldHandle = false;
  }

  if (found == -1)
    return NS_OK;

  end = found;

  rv = aCmdLine->GetLength(&count);
  NS_ENSURE_SUCCESS(rv, rv);

  if (count > found + 1) {
    aCmdLine->GetArgument(found + 1, uristr);
    if (StringBeginsWith(uristr, NS_LITERAL_STRING("mailto:"))  ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("preselectid=")) ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("to="))  ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("cc="))  ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("bcc=")) ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("newsgroups=")) ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("subject=")) ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("format=")) ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("body="))  ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("attachment=")) ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("message=")) ||
        StringBeginsWith(uristr, NS_LITERAL_STRING("from="))) {
      composeShouldHandle = true; // the -url argument looks like mailto
      end++;
      // mailto: URIs are frequently passed with spaces in them. They should be
      // escaped with %20, but we hack around broken clients. See bug 231032.
      while (end + 1 < count) {
        nsAutoString curarg;
        aCmdLine->GetArgument(end + 1, curarg);
        if (curarg.First() == '-')
          break;

        uristr.Append(' ');
        uristr.Append(curarg);
        ++end;
      }
    }
    else {
      uristr.Truncate();
    }
  }
  if (composeShouldHandle)
  {
    aCmdLine->RemoveArguments(found, end);

    nsCOMPtr<nsIWindowWatcher> wwatch (do_GetService(NS_WINDOWWATCHER_CONTRACTID));
    NS_ENSURE_TRUE(wwatch, NS_ERROR_FAILURE);

    nsCOMPtr<nsISupportsString> arg(do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID));
    if (arg)
      arg->SetData(uristr);

    nsCOMPtr<mozIDOMWindowProxy> opened;
    wwatch->OpenWindow(nullptr, DEFAULT_CHROME, "_blank",
                       "chrome,dialog=no,all", arg, getter_AddRefs(opened));

    aCmdLine->SetPreventDefault(true);
  }
  return NS_OK;
}

NS_IMETHODIMP
nsMsgComposeService::GetHelpInfo(nsACString& aResult)
{
  aResult.AssignLiteral(
    "  -compose [ <options> ] Compose a mail or news message. Options are specified\n"
    "                     as string \"option='value,...',option=value,...\" and\n"
    "                     include: from, to, cc, bcc, newsgroups, subject, body,\n"
    "                     message (file), attachment (file), format (html | text).\n"
    "                     Example: \"to=john@example.com,subject='Dinner tonight?'\"\n");
  return NS_OK;
}
