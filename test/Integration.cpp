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

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

#define Z do { if (::testing::Test::HasFatalFailure()) { std::cerr << "At: " << __FILE__ << ":" << __LINE__ << std::endl; return; } } while (0)

using testing::HasSubstr;

namespace {

template <typename Device>
class IO {
public:
	IO(Device* device, bool verbose = false) : m_device(device), m_verbose(verbose) {}
	virtual ~IO() {}
	void ReadUntil(QByteArray pattern) {
		auto deadline = QDateTime::currentDateTime().addSecs(30);
		while (true) {
			int search = m_readed.indexOf(pattern);
			if (search != -1) {
				m_readed.remove(0, search + pattern.length());
				return;
			}
			if (m_readed.length() > pattern.length()) {
				m_readed = m_readed.right(pattern.length());
			}
			const int timeout_ms = QDateTime::currentDateTime().msecsTo(deadline);
			ASSERT_GT(timeout_ms, 0) << "Wanted:" << pattern.toStdString();
			ASSERT_TRUE(m_device->waitForReadyRead(timeout_ms)) << "Wanted: " << pattern.toStdString();
			QByteArray chunk = m_device->readAll();
			if (m_verbose) {
				std::cout << chunk.toStdString() << std::flush;
			}
			m_readed += chunk;
		}
	}
	void Write(QString s = "", bool new_line = true) {
		if (!m_device) return;
		if (m_verbose) {
			std::cout << s.toStdString() << std::flush;
			if (new_line) {
				std::cout << std::endl;
			}
		}
		s += "\n";
		{
			QTextStream str(m_device);
			str << s;
		}
		FlushIfCan(m_device);
	}
	void Close() {
#ifdef __CYGWIN__
#ifdef __x86_64__
		// Qt on cygwin64 silently doesn't send the rest of buffer from socket without this line
		sleep(1);
#endif
#endif
		m_device->disconnectFromHost();
	}

private:
	// QTextStream doesn't flush QTcpSocket, and QIODevice doesn't have flush() at all...
	static void FlushIfCan(QIODevice*) {}
	static void FlushIfCan(QTcpSocket* sock) {
		sock->flush();
	}

	Device* m_device;
	bool m_verbose;
	QByteArray m_readed;
};

template <typename Device>
IO<Device> WrapIO(Device* d) {
	return IO<Device>(d);
}

using Socket = IO<QTcpSocket>;

class Process : public IO<QProcess> {
public:
	Process(QString cmd, QStringList args, bool interactive) : IO(&m_proc, true) {
		if (!interactive) {
			m_proc.setProcessChannelMode(QProcess::ForwardedChannels);
		}
		auto env = QProcessEnvironment::systemEnvironment();
		// Default exit codes of sanitizers upon error:
		// ASAN - 1
		// LSAN - 23 (part of ASAN, but uses a different value)
		// TSAN - 66
		//
		// ZNC uses 1 too to report startup failure.
		// But we don't want to confuse expected starup failure with ASAN error.
		env.insert("ASAN_OPTIONS", "exitcode=57");
		m_proc.setProcessEnvironment(env);
		m_proc.start(cmd, args);
	}
	~Process() {
		if (m_kill) m_proc.terminate();
		[this]() {
			ASSERT_TRUE(m_proc.waitForFinished());
			if (!m_allowDie) {
				ASSERT_EQ(QProcess::NormalExit, m_proc.exitStatus());
				ASSERT_EQ(m_exit, m_proc.exitCode());
			}
		}();
	}
	void ShouldFinishItself(int code = 0) {
		m_kill = false;
		m_exit = code;
	}
	void CanDie() {
		m_allowDie = true;
	}

private:
	bool m_kill = true;
	int m_exit = 0;
	bool m_allowDie = false;
	QProcess m_proc;
};

void WriteConfig(QString path) {
	Process p("./znc", QStringList() << "--debug" << "--datadir" << path << "--makeconf", true);
	p.ReadUntil("Listen on port");Z;          p.Write("12345");
	p.ReadUntil("Listen using SSL");Z;        p.Write();
	p.ReadUntil("IPv6");Z;                    p.Write();
	p.ReadUntil("Username");Z;                p.Write("user");
	p.ReadUntil("password");Z;                p.Write("hunter2", false);
	p.ReadUntil("Confirm");Z;                 p.Write("hunter2", false);
	p.ReadUntil("Nick [user]");Z;             p.Write();
	p.ReadUntil("Alternate nick [user_]");Z;  p.Write();
	p.ReadUntil("Ident [user]");Z;            p.Write();
	p.ReadUntil("Real name");Z;               p.Write();
	p.ReadUntil("Bind host");Z;               p.Write();
	p.ReadUntil("Set up a network?");Z;       p.Write();
	p.ReadUntil("Name [freenode]");Z;         p.Write("test");
	p.ReadUntil("Server host (host only)");Z; p.Write("127.0.0.1");
	p.ReadUntil("Server uses SSL?");Z;        p.Write();
	p.ReadUntil("6667");Z;                    p.Write();
	p.ReadUntil("password");Z;                p.Write();
	p.ReadUntil("channels");Z;                p.Write();
	p.ReadUntil("Launch ZNC now?");Z;         p.Write("no");
	p.ShouldFinishItself();
}

TEST(Config, AlreadyExists) {
	QTemporaryDir dir;
	WriteConfig(dir.path());Z;
	Process p("./znc", QStringList() << "--debug" << "--datadir" << dir.path() << "--makeconf", true);
	p.ReadUntil("already exists");Z;
	p.CanDie();
}

// Can't use QEventLoop without existing QCoreApplication
class App {
public:
	App() : m_argv(new char{}), m_app(m_argc, &m_argv) {}
	~App() { delete m_argv; }
private:
	int m_argc = 1;
	char* m_argv;
	QCoreApplication m_app;
};

class ZNCTest : public testing::Test {
protected:
	void SetUp() override {
		WriteConfig(m_dir.path());Z;
		ASSERT_TRUE(m_server.listen(QHostAddress::LocalHost, 6667)) << m_server.errorString().toStdString();Z;
	}

	Socket ConnectIRCd() {
		[this]{ ASSERT_TRUE(m_server.waitForNewConnection(30000 /* msec */)); }();
		return WrapIO(m_server.nextPendingConnection());
	}

	Socket ConnectClient() {
		m_clients.emplace_back();
		QTcpSocket& sock = m_clients.back();
		sock.connectToHost("127.0.0.1", 12345);
		[&]{ ASSERT_TRUE(sock.waitForConnected()) << sock.errorString().toStdString(); }();
		return WrapIO(&sock);
	}

	std::unique_ptr<Process> Run() {
		return std::unique_ptr<Process>(new Process("./znc", QStringList() << "--debug" << "--datadir" << m_dir.path(), false));
	}

	Socket LoginClient() {
		auto client = ConnectClient();
		client.Write("PASS :hunter2");
		client.Write("NICK nick");
		client.Write("USER user/test x x :x");
		return client;
	}

	std::unique_ptr<QNetworkReply> HttpGet(QNetworkRequest request) {
		return HandleHttp(m_network.get(request));
	}
	std::unique_ptr<QNetworkReply> HttpPost(QNetworkRequest request, QList<QPair<QString, QString>> data) {
		request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
		QUrlQuery q;
		q.setQueryItems(data);
		return HandleHttp(m_network.post(request, q.toString().toUtf8()));
	}
	std::unique_ptr<QNetworkReply> HandleHttp(QNetworkReply* reply) {
		QEventLoop loop;
		QObject::connect(reply, &QNetworkReply::finished, [&]() {
			loop.quit();
		});
		QObject::connect(reply, static_cast<void(QNetworkReply::*)(QNetworkReply::NetworkError)>(&QNetworkReply::error), [&](QNetworkReply::NetworkError e) {
			ADD_FAILURE() << reply->errorString().toStdString();
		});
		QTimer::singleShot(30000 /* msec */, &loop, [&]() {
			ADD_FAILURE() << "connection timeout";
			loop.quit();
		});
		loop.exec();
		return std::unique_ptr<QNetworkReply>(reply);
	}

	App m_app;
	QNetworkAccessManager m_network;
	QTemporaryDir m_dir;
	QTcpServer m_server;
	std::list<QTcpSocket> m_clients;
};

TEST_F(ZNCTest, Connect) {
	auto znc = Run();Z;

	auto ircd = ConnectIRCd();Z;
	ircd.ReadUntil("CAP LS");Z;

	auto client = ConnectClient();Z;
	client.Write("PASS :hunter2");
	client.Write("NICK nick");
	client.Write("USER user/test x x :x");
	client.ReadUntil("Welcome");Z;
	client.Close();

	client = ConnectClient();Z;
	client.Write("PASS :user:hunter2");
	client.Write("NICK nick");
	client.Write("USER u x x x");
	client.ReadUntil("Welcome");Z;
	client.Close();

	client = ConnectClient();Z;
	client.Write("NICK nick");
	client.Write("USER user x x x");
	client.ReadUntil("Configure your client to send a server password");
	client.Close();

	ircd.Write(":server 001 nick :Hello");
	ircd.ReadUntil("WHO");Z;
}

TEST_F(ZNCTest, Channel) {
	auto znc = Run();Z;
	auto ircd = ConnectIRCd();Z;

	auto client = LoginClient();Z;
	client.ReadUntil("Welcome");Z;
	client.Write("JOIN #znc");
	client.Close();

	ircd.Write(":server 001 nick :Hello");
	ircd.ReadUntil("JOIN #znc");Z;
	ircd.Write(":nick JOIN :#znc");
	ircd.Write(":server 353 nick #znc :nick");
	ircd.Write(":server 366 nick #znc :End of /NAMES list");
	ircd.Write(":server PING :1");
	ircd.ReadUntil("PONG 1");

	client = LoginClient();Z;
	client.ReadUntil(":nick JOIN :#znc");Z;
}

TEST_F(ZNCTest, HTTP) {
	auto znc = Run();Z;
	auto ircd = ConnectIRCd();Z;
	auto reply = HttpGet(QNetworkRequest(QUrl("http://127.0.0.1:12345/")));Z;
	EXPECT_THAT(reply->rawHeader("Server").toStdString(), HasSubstr("ZNC"));
}

TEST_F(ZNCTest, FixCVE20149403) {
	auto znc = Run();Z;
	auto ircd = ConnectIRCd();Z;
	ircd.Write(":server 001 nick :Hello");
	ircd.Write(":server 005 nick CHANTYPES=# :supports");
	ircd.Write(":server PING :1");
	ircd.ReadUntil("PONG 1");Z;

	QNetworkRequest request;
	request.setRawHeader("Authorization", "Basic " + QByteArray("user:hunter2").toBase64());
	request.setUrl(QUrl("http://127.0.0.1:12345/mods/global/webadmin/addchan"));
	HttpPost(request, {
		{"user", "user"},
		{"network", "test"},
		{"submitted", "1"},
		{"name", "znc"},
	});
	ircd.ReadUntil("JOIN #znc");Z;
	EXPECT_THAT(HttpPost(request, {
		{"user", "user"},
		{"network", "test"},
		{"submitted", "1"},
		{"name", "znc"},
	})->readAll().toStdString(), HasSubstr("Channel [#znc] already exists"));
}

TEST_F(ZNCTest, FixFixOfCVE20149403) {
	auto znc = Run();Z;
	auto ircd = ConnectIRCd();Z;
	ircd.Write(":server 001 nick :Hello");
	ircd.Write(":nick JOIN @#znc");
	ircd.ReadUntil("MODE @#znc");Z;
	ircd.Write(":server 005 nick STATUSMSG=@ :supports");
	ircd.Write(":server PING :12345");
	ircd.ReadUntil("PONG 12345");Z;

	QNetworkRequest request;
	request.setRawHeader("Authorization", "Basic " + QByteArray("user:hunter2").toBase64());
	request.setUrl(QUrl("http://127.0.0.1:12345/mods/global/webadmin/addchan"));
	auto reply = HttpPost(request, {
		{"user", "user"},
		{"network", "test"},
		{"submitted", "1"},
		{"name", "@#znc"},
	});
	EXPECT_THAT(reply->readAll().toStdString(), HasSubstr("Could not add channel [@#znc]"));
}

TEST_F(ZNCTest, InvalidConfigInChan) {
	QFile conf(m_dir.path() + "/configs/znc.conf");
	ASSERT_TRUE(conf.open(QIODevice::Append | QIODevice::Text));
	QTextStream out(&conf);
	out << R"(
		<User foo>
			<Network bar>
				<Chan #baz>
					Invalid = Line
				</Chan>
			</Network>
		</User>
	)";
	out.flush();
	auto znc = Run();Z;
	znc->ShouldFinishItself(1);
}

TEST_F(ZNCTest, ControlpanelModule) {
	auto znc = Run();Z;
	auto ircd = ConnectIRCd();Z;
	auto client = LoginClient();Z;

	const QString request = "PRIVMSG *controlpanel :";
	const QByteArray response = ":*controlpanel!znc@znc.in PRIVMSG nick :";

	// TODO: Figure out how to check for "HAVE_ICU" to test encoding.
	// TODO: Test the CTCP VERSION.

	// 1. Test everything on "user".
	// 2. Test everything on another user "KindOne" from "user".
	// 3. Test one thing on "foobar" as the user does not exist from "user".

	// Add myself so I can test other things along with "user".
	client.Write(request + "AddUser KindOne hunter2");
	client.ReadUntil(response + "User [KindOne] added!");Z;

	client.Write(request + "AddUser KindOne hunter2");
	client.ReadUntil(response + "Error: User [KindOne] already exists!");Z;

	client.Write(request + "AddCTCP user VERSION Test");
	client.ReadUntil(response + "Added!");Z;

	client.Write(request + "AddNetwork user freenode");
	client.ReadUntil(response + "Network [freenode] added for user [user].");Z;

	client.Write(request + "AddServer user freenode 127.0.0.1 6667");
	client.ReadUntil(response + "Added IRC Server [127.0.0.1 6667] for network [freenode] for user [user].");Z;

	client.Write(request + "AddChan user freenode #znc");
	client.ReadUntil(response + "Channel [#znc] for user [user] added.");Z;

	client.Write(request + "AddChan user freenode #znc");
	client.ReadUntil(response + "Error: [user] already has a channel named [#znc].");Z;

	client.Write(request + "AddUser user hunter2");
	client.ReadUntil(response + "Error: User [user] already exists!");Z;

	client.Write(request + "CloneUser user user_clone");
	client.ReadUntil(response + "User [user_clone] added!");Z;

	client.Write(request + "CloneUser user user_clone");
	client.ReadUntil(response + "Error: User not added! [User already exists]");Z;

	client.Write(request + "DelCTCP user VERSION");
	client.ReadUntil(response + "Successfully removed [VERSION] for user [user].");Z;

	client.Write(request + "DelCTCP user VERSION");
	client.ReadUntil(response + "Error: [VERSION] not found for user [user]!");Z;

	client.Write(request + "DelChan user freenode #znc");
	client.ReadUntil(response + "Channel(s) [#znc] for user [user] deleted.");Z;

	client.Write(request + "DelChan user freenode #znc");
	client.ReadUntil(response + "Error: User [user] does not have any channel matching [#znc].");Z;

	client.Write(request + "DelNetwork freenode");
	client.ReadUntil(response + "Network [freenode] deleted on user [user].");Z;

	client.Write(request + "DelNetwork freenode");
	client.ReadUntil(response + "Error: [user] does not have a network named [freenode].");Z;

	client.Write(request + "AddNetwork user freenode");
	client.ReadUntil(response + "Network [freenode] added for user [user].");Z;

	client.Write(request + "AddChan user freenode #znc");
	client.ReadUntil(response + "Channel [#znc] for user [user] added.");Z;

	client.Write(request + "DelUser user_clone");
	client.ReadUntil(response + "User [user_clone] deleted!");Z;

	client.Write(request + "DelUser user_clone");
	client.ReadUntil(response + "Error: User [user_clone] does not exist!");Z;

	client.Write(request + "Disconnect user freenode");
	client.ReadUntil(response + "Closed IRC connection for network [freenode] on user [user].");Z;

	client.Write(request + "Disconnect user EFnet");
	client.ReadUntil(response + "Error: [user] does not have a network named [EFnet].");Z;

	client.Write(request + "Get Nick");
	client.ReadUntil(response + "Nick = user");Z;

	client.Write(request + "Get Altnick");
	client.ReadUntil(response + "AltNick = user_");Z;

	client.Write(request + "Get Ident");
	client.ReadUntil(response + "Ident = user");Z;

	client.Write(request + "Get RealName");
	client.ReadUntil(response + "RealName = ZNC");Z;

	client.Write(request + "Get BindHost");
	client.ReadUntil(response + "BindHost = ");Z;

	client.Write(request + "Get DefaultChanModes");
	client.ReadUntil(response + "DefaultChanModes = ");Z;

	client.Write(request + "Get QuitMsg");
	client.ReadUntil(response + "QuitMsg = ");Z;

	client.Write(request + "Get Password");
	client.ReadUntil(response + "Error: Unknown variable");Z;

	client.Write(request + "Get Timezone");
	client.ReadUntil(response + "Timezone = ");Z;

	client.Write(request + "Get TimestampFormat");
	client.ReadUntil(response + "TimestampFormat = ");Z;

	client.Write(request + "Get DCCBindHost");
	client.ReadUntil(response + "DCCBindHost = ");Z;

	client.Write(request + "Get StatusPrefix");
	client.ReadUntil(response + "StatusPrefix = *");Z;

	client.Write(request + "Get BufferCount");
	client.ReadUntil(response + "BufferCount = 50");Z;

	client.Write(request + "Get JoinTries");
	client.ReadUntil(response + "JoinTries = ");Z;

	client.Write(request + "Get MaxJoins");
	client.ReadUntil(response + "MaxJoins = ");Z;

	client.Write(request + "Get MaxNetworks");
	client.ReadUntil(response + "MaxNetworks = ");Z;

	client.Write(request + "Get MaxQueryBuffers");
	client.ReadUntil(response + "MaxQueryBuffers = ");Z;

	client.Write(request + "Get Admin");
	client.ReadUntil(response + "Admin = true");Z;

	client.Write(request + "Get AppendTimestamp");
	client.ReadUntil(response + "AppendTimestamp = false");Z;

	client.Write(request + "Get AutoClearChanBuffer");
	client.ReadUntil(response + "AutoClearChanBuffer = true");Z;

	client.Write(request + "Get AutoClearQueryBuffer");
	client.ReadUntil(response + "AutoClearQueryBuffer = true");Z;

	client.Write(request + "Get DenyLoadMod");
	client.ReadUntil(response + "DenyLoadMod = false");Z;

	client.Write(request + "Get DenySetBindHost");
	client.ReadUntil(response + "DenySetBindHost = false");Z;

	client.Write(request + "Get MultiClients");
	client.ReadUntil(response + "MultiClients = true");Z;

	client.Write(request + "Get PrependTimestamp");
	client.ReadUntil(response + "PrependTimestamp = true");Z;

	client.Write(request + "GetChan DefModes user freenode #znc");
	client.ReadUntil(response + "#znc: DefModes = ");Z;

	client.Write(request + "GetChan Key user freenode #znc");
	client.ReadUntil(response + "#znc: Key = ");Z;

	client.Write(request + "GetChan BufferSize user freenode #znc");
	client.ReadUntil(response + "#znc: BufferSize = 50 (default)");Z;

	client.Write(request + "GetChan InConfig user freenode #znc");
	client.ReadUntil(response + "#znc: InConfig = true");Z;

	client.Write(request + "GetChan AutoClearChanBuffer user freenode #znc");
	client.ReadUntil(response + "#znc: AutoClearChanBuffer = true (default)");Z;

	client.Write(request + "GetChan Detached user freenode #znc");
	client.ReadUntil(response + "#znc: Detached = false");Z;

	client.Write(request + "GetNetwork Nick user freenode");
	client.ReadUntil(response + "Nick = user");Z;

	client.Write(request + "GetNetwork Altnick user freenode");
	client.ReadUntil(response + "AltNick = user_");Z;

	client.Write(request + "GetNetwork Ident user freenode");
	client.ReadUntil(response + "Ident = user");Z;

	client.Write(request + "GetNetwork BindHost user freenode");
	client.ReadUntil(response + "BindHost = ");Z;

	client.Write(request + "GetNetwork FloodRate user freenode");
	client.ReadUntil(response + "FloodRate = 1.00");Z;

	client.Write(request + "GetNetwork FloodBurst user freenode");
	client.ReadUntil(response + "FloodBurst = 4");Z;

	client.Write(request + "GetNetwork JoinDelay user freenode");
	client.ReadUntil(response + "JoinDelay = 0");Z;

	client.Write(request + "GetNetwork QuitMsg user freenode");
	client.ReadUntil(response + "QuitMsg = ");Z;

	client.Write(request + "AddCTCP user VERSION Test");
	client.ReadUntil(response + "Added!");Z;

	client.Write(request + "ListCTCPs");
	client.ReadUntil(response + "Request: VERSION");Z;

	client.Write(request + "ListMods");
	client.ReadUntil(response + "Usage: ListMods <username>");Z;

	client.Write(request + "ListNetMods");
	client.ReadUntil(response + "Usage: ListNetMods <username> <network>");Z;

	client.Write(request + "ListNetworks");
	client.ReadUntil(response + "Network: test");Z;

	client.Write(request + "ListUsers");
	client.ReadUntil(response + "Username: user");Z;

	client.Write(request + "LoadModule");
	client.ReadUntil(response + "Usage: LoadModule <username> <modulename> [args]");Z;

	client.Write(request + "LoadModule user");
	client.ReadUntil(response + "Usage: LoadModule <username> <modulename> [args]");Z;

	client.Write(request + "LoadModule user log");
	client.ReadUntil(response + "Loaded module [log]");Z;

	client.Write(request + "LoadModule user log");
	client.ReadUntil(response + "Unable to load module [log] because it is already loaded");Z;

	client.Write(request + "LoadModule user autoop");
	client.ReadUntil(response + "Unable to load module [autoop] [Module [autoop] does not support module type [User].");Z;

	client.Write(request + "LoadNetModule user freenode log");
	client.ReadUntil(response + "Loaded module [log]");Z;

	client.Write(request + "LoadNetModule user freenode log");
	client.ReadUntil(response + "Unable to load module [log] because it is already loaded");Z;

	client.Write(request + "LoadNetModule user EFnet log");
	client.ReadUntil(response + "Error: [user] does not have a network named [EFnet].");Z;

	client.Write(request + "Reconnect user freenode");
	client.ReadUntil(response + "Queued network [freenode] for user [user] for a reconnect.");Z;

	client.Write(request + "Reconnect user EFnet");
	client.ReadUntil(response + "Error: [user] does not have a network named [EFnet].");Z;

	client.Write(request + "Set Nick user user1");
	client.ReadUntil(response + "Nick = user1");Z;

	client.Write(request + "Set Altnick user user_1");
	client.ReadUntil(response + "AltNick = user_1");Z;

	client.Write(request + "Set Ident user user1");
	client.ReadUntil(response + "Ident = user1");Z;

	client.Write(request + "Set RealName user lol");
	client.ReadUntil(response + "RealName = lol");Z;

	client.Write(request + "Set BindHost user 0.0.0.0");
	client.ReadUntil(response + "BindHost = 0.0.0.0");Z;

	client.Write(request + "Set BindHost user 0.0.0.0");
	client.ReadUntil(response + "This bind host is already set!");Z;

	// Need to clear the bindhost for testing in the 'setnetwork'.
	client.Write("PRIVMSG *status :ClearUserBindHost");
	client.ReadUntil(":*status!znc@znc.in PRIVMSG nick :Bind host cleared for your user.");Z;

	client.Write(request + "Set DefaultChanModes user +znst");
	client.ReadUntil(response + "DefaultChanModes = +znst");Z;

	client.Write(request + "Set QuitMsg user Writing this took forever.");
	client.ReadUntil(response + "QuitMsg = Writing this took forever");Z;

	client.Write(request + "Set Password user hunter2");
	client.ReadUntil(response + "Password has been changed!");Z;

	client.Write(request + "Set Timezone user America/New_York");
	client.ReadUntil(response + "Timezone = America/New_York");Z;

	client.Write(request + "Set TimestampFormat user [%H:%M]");
	client.ReadUntil(response + "TimestampFormat = [%H:%M]");Z;

	client.Write(request + "Set DCCBindHost user 0.0.0.0");
	client.ReadUntil(response + "DCCBindHost = 0.0.0.0");Z;

	client.Write(request + "Set StatusPrefix user &");
	client.ReadUntil(":&controlpanel!znc@znc.in PRIVMSG nick :StatusPrefix = &");Z;

	client.Write("PRIVMSG &controlpanel :Set StatusPrefix user *");
	client.ReadUntil(response + "StatusPrefix = *");Z;

	client.Write(request + "SetChan DefModes user freenode #znc ms");
	client.ReadUntil(response + "#znc: DefModes = ms");Z;

	client.Write(request + "SetChan Key user freenode #znc KindOneRules");
	client.ReadUntil(response + "#znc: Key = KindOneRules");Z;

	client.Write(request + "SetChan BufferSize user freenode #znc 9001");
	client.ReadUntil(response + "#znc: BufferSize = 9001");Z;

	client.Write(request + "SetChan InConfig user freenode #znc true");
	client.ReadUntil(response + "#znc: InConfig = true");Z;

	client.Write(request + "SetChan AutoClearChanBuffer user freenode #znc false");
	client.ReadUntil(response + "#znc: AutoClearChanBuffer = false");Z;

	client.Write(request + "SetChan Detached user freenode #znc false");
	client.ReadUntil(response + "#znc: Detached = false");Z;

	client.Write(request + "SetNetwork Nick user freenode NotUser");
	client.ReadUntil(response + "Nick = NotUser");Z;

	client.Write(request + "SetNetwork Altnick user freenode NotUser_");
	client.ReadUntil(response + "AltNick = NotUser_");Z;

	client.Write(request + "SetNetwork Ident user freenode identd");
	client.ReadUntil(response + "Ident = identd");Z;

	client.Write(request + "SetNetwork BindHost user freenode 0.0.0.0");
	client.ReadUntil(response + "BindHost = 0.0.0.0");Z;

	client.Write(request + "SetNetwork FloodRate user freenode 66.00");
	client.ReadUntil(response + "FloodRate = 66.00");Z;

	client.Write(request + "SetNetwork FloodBurst user freenode 55");
	client.ReadUntil(response + "FloodBurst = 55");Z;

	client.Write(request + "SetNetwork JoinDelay user freenode 22");
	client.ReadUntil(response + "JoinDelay = 22");Z;

	client.Write(request + "SetNetwork QuitMsg user freenode telnet");
	client.ReadUntil(response + "QuitMsg = telnet");Z;

	client.Write(request + "Get Nick user");
	client.ReadUntil(response + "Nick = user1");Z;

	client.Write(request + "Get Altnick user");
	client.ReadUntil(response + "AltNick = user_1");Z;

	client.Write(request + "Get Ident user");
	client.ReadUntil(response + "Ident = user1");Z;

	client.Write(request + "Get RealName user");
	client.ReadUntil(response + "RealName = lol");Z;

	client.Write(request + "Get BindHost user");
	client.ReadUntil(response + "BindHost = ");Z;

	client.Write(request + "Get DefaultChanModes user");
	client.ReadUntil(response + "DefaultChanModes = +znst");Z;

	client.Write(request + "Get QuitMsg user");
	client.ReadUntil(response + "QuitMsg = Writing this took forever");Z;

	client.Write(request + "Get Password user");
	client.ReadUntil(response + "Error: Unknown variable");Z;

	client.Write(request + "Get Timezone user");
	client.ReadUntil(response + "Timezone = America/New_York");Z;

	client.Write(request + "Get TimestampFormat user");
	client.ReadUntil(response + "TimestampFormat = [%H:%M]");Z;

	client.Write(request + "Get DCCBindHost user");
	client.ReadUntil(response + "DCCBindHost = 0.0.0.0");Z;

	client.Write(request + "Get StatusPrefix user");
	client.ReadUntil(response + "StatusPrefix = *");Z;

	client.Write(request + "GetChan DefModes user freenode #znc");
	client.ReadUntil(response + "#znc: DefModes = ms");Z;

	client.Write(request + "GetChan Key user freenode #znc");
	client.ReadUntil(response + "#znc: Key = KindOneRules");Z;

	client.Write(request + "GetChan BufferSize user freenode #znc");
	client.ReadUntil(response + "#znc: BufferSize = 9001");Z;

	client.Write(request + "GetChan InConfig user freenode #znc");
	client.ReadUntil(response + "#znc: InConfig = true");Z;

	client.Write(request + "GetChan AutoClearChanBuffer user freenode #znc");
	client.ReadUntil(response + "#znc: AutoClearChanBuffer = false");Z;

	client.Write(request + "GetChan Detached user freenode #znc");
	client.ReadUntil(response + "#znc: Detached = false");Z;

	client.Write(request + "GetNetwork Nick user freenode");
	client.ReadUntil(response + "Nick = NotUser");Z;

	client.Write(request + "GetNetwork Altnick user freenode");
	client.ReadUntil(response + "AltNick = NotUser_");Z;

	client.Write(request + "GetNetwork Ident user freenode");
	client.ReadUntil(response + "Ident = identd");Z;

	client.Write(request + "GetNetwork BindHost user freenode");
	client.ReadUntil(response + "BindHost = 0.0.0.0");Z;

	client.Write(request + "GetNetwork FloodRate user freenode");
	client.ReadUntil(response + "FloodRate = 66.00");Z;

	client.Write(request + "GetNetwork FloodBurst user freenode 55");
	client.ReadUntil(response + "FloodBurst = 55");Z;

	client.Write(request + "GetNetwork JoinDelay user freenode 22");
	client.ReadUntil(response + "JoinDelay = 22");Z;

	client.Write(request + "GetNetwork QuitMsg user freenode");
	client.ReadUntil(response + "QuitMsg = telnet");Z;

	client.Write(request + "UnLoadModule");
	client.ReadUntil(response + "Usage: UnloadModule <username> <modulename>");Z;

	client.Write(request + "UnLoadModule user");
	client.ReadUntil(response + "Usage: UnloadModule <username> <modulename>");Z;

	// https://github.com/znc/znc/issues/194
	client.Write(request + "UnloadModule user controlpanel");
	client.ReadUntil(response + "Please use /znc unloadmod controlpanel");Z;

	client.Write(request + "UnLoadModule user log");
	client.ReadUntil(response + "Unloaded module [log]");Z;

	client.Write(request + "UnLoadModule user log");
	client.ReadUntil(response + "Unable to unload module [log] [Module [log] not loaded.]");Z;

	client.Write(request + "UnLoadNetModule user freenode log");
	client.ReadUntil(response + "Unloaded module [log]");Z;

	client.Write(request + "UnLoadNetModule user freenode log");
	client.ReadUntil(response + "Unable to unload module [log] [Module [log] not loaded.]");Z;

	client.Write(request + "UnLoadNetModule user EFnet log");
	client.ReadUntil(response + "Error: [user] does not have a network named [EFnet].");Z;

	// Test on second user "KindOne" from "user".

	client.Write(request + "AddCTCP KindOne VERSION Test");
	client.ReadUntil(response + "Added!");Z;

	// Added at the very beginning.
	client.Write(request + "AddNetwork KindOne freenode");
	client.ReadUntil(response + "Network [freenode] added for user [KindOne].");Z;

	client.Write(request + "AddServer KindOne freenode 127.0.0.1 6667");
	client.ReadUntil(response + "Added IRC Server [127.0.0.1 6667] for network [freenode] for user [KindOne].");Z;

	client.Write(request + "AddChan KindOne freenode #znc");
	client.ReadUntil(response + "Channel [#znc] for user [KindOne] added.");Z;

	client.Write(request + "AddChan KindOne freenode #znc");
	client.ReadUntil(response + "Error: [KindOne] already has a channel named [#znc].");Z;

	client.Write(request + "AddUser KindOne hunter2");
	client.ReadUntil(response + "Error: User [KindOne] already exists!");Z;

	client.Write(request + "CloneUser KindOne KindOne_clone");
	client.ReadUntil(response + "User [KindOne_clone] added!");Z;

	client.Write(request + "CloneUser KindOne KindOne_clone");
	client.ReadUntil(response + "Error: User not added! [User already exists]");Z;

	client.Write(request + "DelCTCP KindOne VERSION");
	client.ReadUntil(response + "Successfully removed [VERSION] for user [KindOne].");Z;

	client.Write(request + "DelCTCP KindOne VERSION");
	client.ReadUntil(response + "Error: [VERSION] not found for user [KindOne]!");Z;

	client.Write(request + "DelChan KindOne freenode #znc");
	client.ReadUntil(response + "Channel(s) [#znc] for user [KindOne] deleted.");Z;

	client.Write(request + "DelChan KindOne freenode #znc");
	client.ReadUntil(response + "Error: User [KindOne] does not have any channel matching [#znc].");Z;

	client.Write(request + "DelNetwork KindOne freenode");
	client.ReadUntil(response + "Network [freenode] deleted on user [KindOne].");Z;

	client.Write(request + "DelNetwork KindOne freenode");
	client.ReadUntil(response + "Error: [KindOne] does not have a network named [freenode].");Z;

	client.Write(request + "AddNetwork KindOne freenode");
	client.ReadUntil(response + "Network [freenode] added for user [KindOne].");Z;

	client.Write(request + "AddChan KindOne freenode #znc");
	client.ReadUntil(response + "Channel [#znc] for user [KindOne] added.");Z;

	client.Write(request + "DelUser KindOne_clone");
	client.ReadUntil(response + "User [KindOne_clone] deleted!");Z;

	client.Write(request + "DelUser KindOne_clone");
	client.ReadUntil(response + "Error: User [KindOne_clone] does not exist!");Z;

	client.Write(request + "Disconnect KindOne freenode");
	client.ReadUntil(response + "Closed IRC connection for network [freenode] on user [KindOne].");Z;

	client.Write(request + "Disconnect KindOne EFnet");
	client.ReadUntil(response + "Error: [KindOne] does not have a network named [EFnet].");Z;

	client.Write(request + "Get Nick KindOne");
	client.ReadUntil(response + "Nick = KindOne");Z;

	client.Write(request + "Get AltNick KindOne");
	client.ReadUntil(response + "AltNick = KindOne");Z;

	client.Write(request + "Get Ident KindOne");
	client.ReadUntil(response + "Ident = KindOne");Z;

	client.Write(request + "Get RealName KindOne");
	client.ReadUntil(response + "RealName = ");Z;

	client.Write(request + "Get BindHost KindOne");
	client.ReadUntil(response + "BindHost = ");Z;

	client.Write(request + "Get DefaultChanModes KindOne");
	client.ReadUntil(response + "DefaultChanModes = ");Z;

	client.Write(request + "Get QuitMsg KindOne");
	client.ReadUntil(response + "QuitMsg = ");Z;

	client.Write(request + "Get Password KindOne");
	client.ReadUntil(response + "Error: Unknown variable");Z;

	client.Write(request + "Get Timezone KindOne");
	client.ReadUntil(response + "Timezone = ");Z;

	client.Write(request + "Get TimestampFormat KindOne");
	client.ReadUntil(response + "TimestampFormat = ");Z;

	client.Write(request + "Get DCCBindHost KindOne");
	client.ReadUntil(response + "DCCBindHost = ");Z;

	client.Write(request + "Get StatusPrefix KindOne");
	client.ReadUntil(response + "StatusPrefix = *");Z;

	client.Write(request + "Get BufferCount KindOne");
	client.ReadUntil(response + "BufferCount = 50");Z;

	client.Write(request + "Get JoinTries KindOne");
	client.ReadUntil(response + "JoinTries = ");Z;

	client.Write(request + "Get MaxJoins KindOne");
	client.ReadUntil(response + "MaxJoins = ");Z;

	client.Write(request + "Get MaxNetworks KindOne");
	client.ReadUntil(response + "MaxNetworks = ");Z;

	client.Write(request + "Get MaxQueryBuffers KindOne");
	client.ReadUntil(response + "MaxQueryBuffers = ");Z;

	client.Write(request + "Get Admin KindOne");
	client.ReadUntil(response + "Admin = false");Z;

	client.Write(request + "Get AppendTimestamp KindOne");
	client.ReadUntil(response + "AppendTimestamp = false");Z;

	client.Write(request + "Get AutoClearChanBuffer KindOne");
	client.ReadUntil(response + "AutoClearChanBuffer = true");Z;

	client.Write(request + "Get AutoClearQueryBuffer KindOne");
	client.ReadUntil(response + "AutoClearQueryBuffer = true");Z;

	client.Write(request + "Get DenyLoadMod KindOne");
	client.ReadUntil(response + "DenyLoadMod = false");Z;

	client.Write(request + "Get DenySetBindHost KindOne");
	client.ReadUntil(response + "DenySetBindHost = false");Z;

	client.Write(request + "Get MultiClients KindOne");
	client.ReadUntil(response + "MultiClients = true");Z;

	client.Write(request + "Get PrependTimestamp KindOne");
	client.ReadUntil(response + "PrependTimestamp = true");Z;

	client.Write(request + "GetChan DefModes KindOne freenode #znc");
	client.ReadUntil(response + "#znc: DefModes = ");Z;

	client.Write(request + "GetChan Key KindOne freenode #znc");
	client.ReadUntil(response + "#znc: Key = ");Z;

	client.Write(request + "GetChan BufferSize KindOne freenode #znc");
	client.ReadUntil(response + "#znc: BufferSize = 50 (default)");Z;

	client.Write(request + "GetChan InConfig KindOne freenode #znc");
	client.ReadUntil(response + "#znc: InConfig = true");Z;

	client.Write(request + "GetChan AutoClearChanBuffer KindOne freenode #znc");
	client.ReadUntil(response + "#znc: AutoClearChanBuffer = true (default)");Z;

	client.Write(request + "GetChan Detached KindOne freenode #znc");
	client.ReadUntil(response + "#znc: Detached = false");Z;

	client.Write(request + "GetNetwork Nick KindOne freenode");
	client.ReadUntil(response + "Nick = KindOne");Z;

	client.Write(request + "GetNetwork Altnick KindOne freenode");
	client.ReadUntil(response + "AltNick = KindOne");Z;

	client.Write(request + "GetNetwork Ident KindOne freenode");
	client.ReadUntil(response + "Ident = KindOne");Z;

	client.Write(request + "GetNetwork BindHost KindOne freenode");
	client.ReadUntil(response + "BindHost = ");Z;

	client.Write(request + "GetNetwork FloodRate KindOne freenode");
	client.ReadUntil(response + "FloodRate = 1.00");Z;

	client.Write(request + "GetNetwork FloodBurst KindOne freenode");
	client.ReadUntil(response + "FloodBurst = 4");Z;

	client.Write(request + "GetNetwork JoinDelay KindOne freenode");
	client.ReadUntil(response + "JoinDelay = 0");Z;

	client.Write(request + "GetNetwork QuitMsg KindOne freenode");
	client.ReadUntil(response + "QuitMsg = ");Z;

	client.Write(request + "AddCTCP KindOne VERSION Test");
	client.ReadUntil(response + "Added!");Z;

	client.Write(request + "ListCTCPs KindOne");
	client.ReadUntil(response + "Request: VERSION");Z;

	client.Write(request + "ListMods KindOne");
	client.ReadUntil(response + "User [KindOne] has no modules loaded.");Z;

	client.Write(request + "LoadModule KindOne autoop");
	client.ReadUntil(response + "Unable to load module [autoop] [Module [autoop] does not support module type [User].]");Z;

	client.Write(request + "LoadModule KindOne perform");
	client.ReadUntil(response + "Loaded module [perform]");Z;

	client.Write(request + "ListMods KindOne");
	client.ReadUntil(response + "Name: perform");Z;

	client.Write(request + "ListNetMods KindOne");
	client.ReadUntil(response + "Usage: ListNetMods <username> <network>");Z;

	client.Write(request + "ListNetworks KindOne");
	client.ReadUntil(response + "Network: freenode");Z;

	client.Write(request + "ListUsers");
	client.ReadUntil(response + "Username: user");Z;

	client.Write(request + "LoadModule");
	client.ReadUntil(response + "Usage: LoadModule <username> <modulename> [args]");Z;

	client.Write(request + "LoadModule KindOne");
	client.ReadUntil(response + "Usage: LoadModule <username> <modulename> [args]");Z;

	client.Write(request + "LoadModule KindOne log");
	client.ReadUntil(response + "Loaded module [log]");Z;

	client.Write(request + "LoadModule KindOne log");
	client.ReadUntil(response + "Unable to load module [log] because it is already loaded");Z;

	client.Write(request + "LoadNetModule KindOne freenode log");
	client.ReadUntil(response + "Loaded module [log]");Z;

	client.Write(request + "LoadNetModule KindOne freenode log");
	client.ReadUntil(response + "Unable to load module [log] because it is already loaded");Z;

	client.Write(request + "LoadNetModule KindOne EFnet log");
	client.ReadUntil(response + "Error: [KindOne] does not have a network named [EFnet].");Z;

	client.Write(request + "Reconnect KindOne freenode");
	client.ReadUntil(response + "Queued network [freenode] for user [KindOne] for a reconnect.");Z;

	client.Write(request + "Reconnect KindOne EFnet");
	client.ReadUntil(response + "Error: [KindOne] does not have a network named [EFnet].");Z;

	client.Write(request + "Set Nick KindOne KindTwo");
	client.ReadUntil(response + "Nick = KindTwo");Z;

	client.Write(request + "Set Altnick KindOne KindTwo_");
	client.ReadUntil(response + "AltNick = KindTwo_");Z;

	client.Write(request + "Set Ident KindOne znc");
	client.ReadUntil(response + "Ident = znc");Z;

	client.Write(request + "Set RealName KindOne real_name");
	client.ReadUntil(response + "RealName = real_name");Z;

	client.Write(request + "Set BindHost KindOne 0.0.0.0");
	client.ReadUntil(response + "BindHost = 0.0.0.0");Z;

	client.Write(request + "Set DefaultChanModes KindOne +inst");
	client.ReadUntil(response + "DefaultChanModes = +inst");Z;

	client.Write(request + "Set QuitMsg KindOne Writing this took forever.");
	client.ReadUntil(response + "QuitMsg = Writing this took forever");Z;

	client.Write(request + "Set Password KindOne hunter2");
	client.ReadUntil(response + "Password has been changed!");Z;

	client.Write(request + "Set Timezone KindOne America/New_York");
	client.ReadUntil(response + "Timezone = America/New_York");Z;

	client.Write(request + "Set TimestampFormat KindOne [%H:%M]");
	client.ReadUntil(response + "TimestampFormat = [%H:%M]");Z;

	client.Write(request + "Set DCCBindHost KindOne 0.0.0.0");
	client.ReadUntil(response + "DCCBindHost = 0.0.0.0");Z;

	client.Write(request + "Set StatusPrefix KindOne &");
	client.ReadUntil(response + "StatusPrefix = &");Z;

	client.Write(request + "Set StatusPrefix KindOne *");
	client.ReadUntil(response + "StatusPrefix = *");Z;

	client.Write(request + "SetChan DefModes KindOne freenode #znc is");
	client.ReadUntil(response + "#znc: DefModes = is");Z;

	client.Write(request + "SetChan Key KindOne freenode #znc KindOneRules");
	client.ReadUntil(response + "#znc: Key = KindOneRules");Z;

	client.Write(request + "SetChan BufferSize KindOne freenode #znc 9001");
	client.ReadUntil(response + "#znc: BufferSize = 9001");Z;

	client.Write(request + "SetChan InConfig KindOne freenode #znc true");
	client.ReadUntil(response + "#znc: InConfig = true");Z;

	client.Write(request + "SetChan AutoClearChanBuffer KindOne freenode #znc false");
	client.ReadUntil(response + "#znc: AutoClearChanBuffer = false");Z;

	client.Write(request + "SetChan Detached KindOne freenode #znc false");
	client.ReadUntil(response + "#znc: Detached = false");Z;

	client.Write(request + "SetNetwork Nick KindOne freenode KindTwo");
	client.ReadUntil(response + "Nick = KindTwo");Z;

	client.Write(request + "SetNetwork Altnick KindOne freenode KindTwo_");
	client.ReadUntil(response + "AltNick = KindTwo_");Z;

	client.Write(request + "SetNetwork Ident KindOne freenode znc");
	client.ReadUntil(response + "Ident = znc");Z;

	client.Write(request + "SetNetwork BindHost KindOne freenode 0.0.0.0");
	client.ReadUntil(response + "This bind host is already set!");Z;

	client.Write(request + "SetNetwork FloodRate KindOne freenode 42.00");
	client.ReadUntil(response + "FloodRate = 42.00");Z;

	client.Write(request + "SetNetwork FloodBurst KindOne freenode 20");
	client.ReadUntil(response + "FloodBurst = 20");Z;

	client.Write(request + "SetNetwork JoinDelay KindOne freenode 5");
	client.ReadUntil(response + "JoinDelay = 5");Z;

	client.Write(request + "SetNetwork QuitMsg KindOne freenode telnet");
	client.ReadUntil(response + "QuitMsg = telnet");Z;

	client.Write(request + "Get Nick KindOne");
	client.ReadUntil(response + "Nick = KindTwo");Z;

	client.Write(request + "Get Altnick KindOne");
	client.ReadUntil(response + "AltNick = KindTwo_");Z;

	client.Write(request + "Get Ident KindOne");
	client.ReadUntil(response + "Ident = znc");Z;

	client.Write(request + "Get RealName KindOne");
	client.ReadUntil(response + "RealName = real_name");Z;

	client.Write(request + "Get BindHost KindOne");
	client.ReadUntil(response + "BindHost = 0.0.0.0");Z;

	client.Write(request + "Get DefaultChanModes KindOne");
	client.ReadUntil(response + "DefaultChanModes = +inst");Z;

	client.Write(request + "Get QuitMsg KindOne");
	client.ReadUntil(response + "QuitMsg = Writing this took forever");Z;

	client.Write(request + "Get Password KindOne");
	client.ReadUntil(response + "Error: Unknown variable");Z;

	client.Write(request + "Get Timezone KindOne");
	client.ReadUntil(response + "Timezone = America/New_York");Z;

	client.Write(request + "Get TimestampFormat KindOne");
	client.ReadUntil(response + "TimestampFormat = [%H:%M]");Z;

	client.Write(request + "Get DCCBindHost KindOne");
	client.ReadUntil(response + "DCCBindHost = 0.0.0.0");Z;

	client.Write(request + "Get StatusPrefix KindOne");
	client.ReadUntil(response + "StatusPrefix = *");Z;

	client.Write(request + "GetChan DefModes KindOne freenode #znc");
	client.ReadUntil(response + "#znc: DefModes = is");Z;

	client.Write(request + "GetChan Key KindOne freenode #znc");
	client.ReadUntil(response + "#znc: Key = KindOneRules");Z;

	client.Write(request + "GetChan BufferSize KindOne freenode #znc");
	client.ReadUntil(response + "#znc: BufferSize = 9001");Z;

	client.Write(request + "GetChan InConfig KindOne freenode #znc");
	client.ReadUntil(response + "#znc: InConfig = true");Z;

	client.Write(request + "GetChan AutoClearChanBuffer KindOne freenode #znc");
	client.ReadUntil(response + "#znc: AutoClearChanBuffer = false");Z;

	client.Write(request + "GetChan Detached KindOne freenode #znc");
	client.ReadUntil(response + "#znc: Detached = false");Z;

	client.Write(request + "GetNetwork Nick KindOne freenode");
	client.ReadUntil(response + "Nick = KindTwo");Z;

	client.Write(request + "GetNetwork Altnick KindOne freenode");
	client.ReadUntil(response + "AltNick = KindTwo_");Z;

	client.Write(request + "GetNetwork Ident KindOne freenode");
	client.ReadUntil(response + "Ident = znc");Z;

	client.Write(request + "GetNetwork BindHost KindOne freenode");
	client.ReadUntil(response + "BindHost = 0.0.0.0");Z;

	client.Write(request + "GetNetwork FloodRate KindOne freenode");
	client.ReadUntil(response + "FloodRate = 42.00");Z;

	client.Write(request + "GetNetwork FloodBurst KindOne freenode");
	client.ReadUntil(response + "FloodBurst = 20");Z;

	client.Write(request + "GetNetwork JoinDelay KindOne freenode");
	client.ReadUntil(response + "JoinDelay = 5");Z;

	client.Write(request + "GetNetwork QuitMsg KindOne freenode");
	client.ReadUntil(response + "QuitMsg = telnet");Z;

	client.Write(request + "UnLoadModule");
	client.ReadUntil(response + "Usage: UnloadModule <username> <modulename>");Z;

	client.Write(request + "UnLoadModule KindOne");
	client.ReadUntil(response + "Usage: UnloadModule <username> <modulename>");Z;

	client.Write(request + "UnLoadModule KindOne log");
	client.ReadUntil(response + "Unloaded module [log]");Z;

	client.Write(request + "UnLoadModule KindOne log");
	client.ReadUntil(response + "Unable to unload module [log] [Module [log] not loaded.]");Z;

	client.Write(request + "UnLoadNetModule KindOne freenode log");
	client.ReadUntil(response + "Unloaded module [log]");Z;

	client.Write(request + "UnLoadNetModule KindOne freenode log");
	client.ReadUntil(response + "Unable to unload module [log] [Module [log] not loaded.]");Z;

	client.Write(request + "UnLoadNetModule KindOne EFnet log");
	client.ReadUntil(response + "Error: [KindOne] does not have a network named [EFnet].");Z;

	// Test on "foobar", a user that does not exist.

	client.Write(request + "AddCTCP foobar VERSION Test");
	client.ReadUntil(response + "Error: User [foobar] not found.");Z;

}

TEST_F(ZNCTest, ShellModule) {
	auto znc = Run();Z;
	auto ircd = ConnectIRCd();Z;
	auto client = LoginClient();Z;
	client.Write("znc loadmod shell");
	client.Write("PRIVMSG *shell :echo blahblah");
	client.ReadUntil("PRIVMSG nick :blahblah");
	client.ReadUntil("PRIVMSG nick :znc$");
}

}  // namespace
