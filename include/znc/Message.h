/*
 * Copyright (C) 2004-2015 ZNC, see the NOTICE file for details.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ZNC_MESSAGE_H
#define ZNC_MESSAGE_H

#include <znc/zncconfig.h>
#include <znc/ZNCString.h>
#include <znc/Nick.h>
#include <sys/time.h>

class CChan;
class CClient;
class CIRCNetwork;

class CMessage {
public:
	explicit CMessage(const CString& sMessage = "");
	CMessage(const CNick& Nick, const CString& sCommand, const VCString& vsParams = VCString(), const MCString& mssTags = MCString::EmptyMap);

	enum class Type {
		Unknown,
		Account,
		Action,
		Away,
		Capability,
		CTCP,
		Error,
		Invite,
		Join,
		Kick,
		Mode,
		Nick,
		Notice,
		Numeric,
		Part,
		Ping,
		Pong,
		Quit,
		Text,
		Topic,
		Wallops,
	};
	Type GetType() const { return m_eType; }

	bool Equals(const CMessage& Other) const;
	void Clone(const CMessage& Other);

	// ZNC <-> IRC
	CIRCNetwork* GetNetwork() const { return m_pNetwork; }
	void SetNetwork(CIRCNetwork* pNetwork) { m_pNetwork = pNetwork; }

	// ZNC <-> CLI
	CClient* GetClient() const { return m_pClient; }
	void SetClient(CClient* pClient) { m_pClient = pClient; }

	CChan* GetChan() const { return m_pChan; }
	void SetChan(CChan* pChan) { m_pChan = pChan; }

	CNick& GetNick() { return m_Nick; }
	const CNick& GetNick() const { return m_Nick; }
	void SetNick(const CNick& Nick) { m_Nick = Nick; }

	const CString& GetCommand() const { return m_sCommand; }
	void SetCommand(const CString& sCommand);

	const VCString& GetParams() const { return m_vsParams; }
	CString GetParams(unsigned int uIdx, unsigned int uLen = -1) const;
	void SetParams(const VCString& vsParams);

	CString GetParam(unsigned int uIdx) const;
	void SetParam(unsigned int uIdx, const CString& sParam);

	const timeval& GetTime() const { return m_time; }
	void SetTime(const timeval& ts) { m_time = ts; }

	const MCString& GetTags() const { return m_mssTags; }
	void SetTags(const MCString& mssTags) { m_mssTags = mssTags; }

	CString GetTag(const CString& sKey) const;
	void SetTag(const CString& sKey, const CString& sValue);

	enum FormatFlags {
		IncludeAll = 0x0,
		ExcludePrefix = 0x1,
		ExcludeTags = 0x2
	};

	CString ToString(unsigned int uFlags = IncludeAll) const;
	void Parse(CString sMessage);

	// Implicit and explicit conversion to a subclass reference.
#ifndef SWIG
	template <typename M>
	M& As() & {
		static_assert(sizeof(M) == sizeof(CMessage), "No data members allowed in CMessage subclasses.");
		return static_cast<M&>(*this);
	}

	template <typename M>
	const M& As() const & {
		static_assert(sizeof(M) == sizeof(CMessage), "No data members allowed in CMessage subclasses.");
		return static_cast<const M&>(*this);
	}

	template <typename M>
	operator M&() & {
		return As<M>();
	}
	template <typename M>
	operator const M&() const & {
		return As<M>();
	}
	// REGISTER_ZNC_MESSAGE allows SWIG to instantiate correct .As<> calls.
#define REGISTER_ZNC_MESSAGE(M)
#else
	// SWIG doesn't parse ref-qualifiers, and doesn't differentiate constness.
	template <typename M> M& As();
#endif

private:
	void InitTime();
	void InitType();

	CNick        m_Nick;
	CString      m_sCommand;
	VCString     m_vsParams;
	MCString     m_mssTags;
	timeval      m_time;
	CIRCNetwork* m_pNetwork = nullptr;
	CClient*     m_pClient = nullptr;
	CChan*       m_pChan = nullptr;
	Type         m_eType = Type::Unknown;
	bool         m_bColon = false;
};

// The various CMessage subclasses are "mutable views" to the data held by CMessage.
// They provide convenient access to message type speficic attributes, but are not
// allowed to hold extra data of their own.
class CTargetMessage : public CMessage {
public:
	CString GetTarget() const { return GetParam(0); }
	void SetTarget(const CString& sTarget) { SetParam(0, sTarget); }
};
REGISTER_ZNC_MESSAGE(CTargetMessage);

class CActionMessage : public CTargetMessage {
public:
	CString GetText() const { return GetParam(1).TrimPrefix_n("\001ACTION ").TrimSuffix_n("\001"); }
	void SetText(const CString& sText) { SetParam(1, "\001ACTION " + sText + "\001"); }
};
REGISTER_ZNC_MESSAGE(CActionMessage);

class CCTCPMessage : public CTargetMessage {
public:
	bool IsReply() const { return GetCommand().Equals("NOTICE"); }
	CString GetText() const { return GetParam(1).TrimPrefix_n("\001").TrimSuffix_n("\001"); }
	void SetText(const CString& sText) { SetParam(1, "\001" + sText + "\001"); }
};
REGISTER_ZNC_MESSAGE(CCTCPMessage);

class CJoinMessage : public CTargetMessage {
public:
	CString GetKey() const { return GetParam(1); }
	void SetKey(const CString& sKey) { SetParam(1, sKey); }
};
REGISTER_ZNC_MESSAGE(CJoinMessage);

class CModeMessage : public CTargetMessage {
public:
	CString GetModes() const { return GetParams(1).TrimPrefix_n(":"); }
};
REGISTER_ZNC_MESSAGE(CModeMessage);

class CNickMessage : public CMessage {
public:
	CString GetOldNick() const { return GetNick().GetNick(); }
	CString GetNewNick() const { return GetParam(0); }
	void SetNewNick(const CString& sNick) { SetParam(0, sNick); }
};
REGISTER_ZNC_MESSAGE(CNickMessage);

class CNoticeMessage : public CTargetMessage {
public:
	CString GetText() const { return GetParam(1); }
	void SetText(const CString& sText) { SetParam(1, sText); }
};
REGISTER_ZNC_MESSAGE(CNoticeMessage);

class CNumericMessage : public CMessage {
public:
	unsigned int GetCode() const { return GetCommand().ToUInt(); }
};
REGISTER_ZNC_MESSAGE(CNumericMessage);

class CKickMessage : public CTargetMessage {
public:
	CString GetKickedNick() const { return GetParam(1); }
	void SetKickedNick(const CString& sNick) { SetParam(1, sNick); }
	CString GetReason() const { return GetParam(2); }
	void SetReason(const CString& sReason) { SetParam(2, sReason); }
};
REGISTER_ZNC_MESSAGE(CKickMessage);

class CPartMessage : public CTargetMessage {
public:
	CString GetReason() const { return GetParam(1); }
	void SetReason(const CString& sReason) { SetParam(1, sReason); }
};
REGISTER_ZNC_MESSAGE(CPartMessage);

class CQuitMessage : public CMessage {
public:
	CString GetReason() const { return GetParam(0); }
	void SetReason(const CString& sReason) { SetParam(0, sReason); }
};
REGISTER_ZNC_MESSAGE(CQuitMessage);

class CTextMessage : public CTargetMessage {
public:
	CString GetText() const { return GetParam(1); }
	void SetText(const CString& sText) { SetParam(1, sText); }
};
REGISTER_ZNC_MESSAGE(CTextMessage);

class CTopicMessage : public CTargetMessage {
public:
	CString GetTopic() const { return GetParam(1); }
	void SetTopic(const CString& sTopic) { SetParam(1, sTopic); }
};
REGISTER_ZNC_MESSAGE(CTopicMessage);

#endif // !ZNC_MESSAGE_H
