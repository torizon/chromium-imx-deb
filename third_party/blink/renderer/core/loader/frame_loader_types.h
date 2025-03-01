/*
 * Copyright (C) 2006 Apple Computer, Inc.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_FRAME_LOADER_TYPES_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_FRAME_LOADER_TYPES_H_

namespace blink {

enum ShouldSendReferrer { kMaybeSendReferrer, kNeverSendReferrer };

enum LoadStartType {
  kNavigationToDifferentDocument,
  kNavigationWithinSameDocument
};

enum class SavePreviousDocumentResources {
  kNever,
  kUntilOnDOMContentLoaded,
  kUntilOnLoad
};

// This enum is used to index different kinds of single-page-application
// navigations for UMA enum histogram. New enum values can be added, but
// existing enums must never be renumbered or deleted and reused.
// This enum should be consistent with SinglePageAppNavigationType in
// tools/metrics/histograms/enums.xml.
enum SinglePageAppNavigationType {
  kSPANavTypeHistoryPushStateOrReplaceState = 0,
  kSPANavTypeSameDocumentBackwardOrForward = 1,
  kSPANavTypeOtherFragmentNavigation = 2,
  kSPANavTypeNavigationApiTransitionWhile = 3,
  kSPANavTypeCount
};

enum class ClientNavigationReason {
  kFormSubmissionGet,
  kFormSubmissionPost,
  kAnchorClick,
  kHttpHeaderRefresh,
  kFrameNavigation,
  kMetaTagRefresh,
  kPageBlock,
  kReload,
  kNone
};

enum class CommitReason {
  // Committing initial empty document.
  kInitialization,
  // Committing navigation as a result of javascript URL execution.
  kJavascriptUrl,
  // Committing a replacement document from XSLT.
  kXSLT,
  // All other navigations.
  kRegular
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_LOADER_FRAME_LOADER_TYPES_H_
