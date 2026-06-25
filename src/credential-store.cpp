#include "credential-store.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <keychain.h>

namespace {

const QString kService = QStringLiteral("com.subsplash.obs-subsplash-sync");

/* Run a QtKeychain job to completion on a nested event loop. The job must
 * outlive the loop, so callers allocate it on the stack with autoDelete off.
 * The nested loop processes queued events, so this must run on the GUI thread
 * (where all callers live) to keep the threading/reentrancy contract explicit. */
void run_job(QKeychain::Job &job)
{
	Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
	job.setAutoDelete(false);
	QEventLoop loop;
	QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
	job.start();
	loop.exec();
}

} // namespace

namespace cred_store {

const QString kClientId = QStringLiteral("client_id");
const QString kClientSecret = QStringLiteral("client_secret");
const QString kAppKey = QStringLiteral("app_key");

bool Read(const QString &key, QString &out_value, QString *out_error)
{
	QKeychain::ReadPasswordJob job(kService);
	job.setKey(key);
	run_job(job);

	if (job.error() == QKeychain::NoError) {
		out_value = job.textData();
		return true;
	}
	/* A missing entry is an expected "no value" result, not a hard error. */
	if (job.error() != QKeychain::EntryNotFound && out_error)
		*out_error = job.errorString();
	return false;
}

bool Write(const QString &key, const QString &value, QString *out_error)
{
	QKeychain::WritePasswordJob job(kService);
	job.setKey(key);
	job.setTextData(value);
	run_job(job);

	if (job.error() == QKeychain::NoError)
		return true;
	if (out_error)
		*out_error = job.errorString();
	return false;
}

bool Remove(const QString &key, QString *out_error)
{
	QKeychain::DeletePasswordJob job(kService);
	job.setKey(key);
	run_job(job);

	if (job.error() == QKeychain::NoError || job.error() == QKeychain::EntryNotFound)
		return true;
	if (out_error)
		*out_error = job.errorString();
	return false;
}

} // namespace cred_store
