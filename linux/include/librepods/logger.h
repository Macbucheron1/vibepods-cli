#pragma once

#include <QDebug>
#include <QLoggingCategory>

Q_DECLARE_LOGGING_CATEGORY(vibepods)

#define LOG_INFO(msg) qCInfo(vibepods) << "\033[32m" << msg << "\033[0m"
#define LOG_WARN(msg) qCWarning(vibepods) << "\033[33m" << msg << "\033[0m"
#define LOG_ERROR(msg) qCCritical(vibepods) << "\033[31m" << msg << "\033[0m"
#define LOG_DEBUG(msg) qCDebug(vibepods) << "\033[34m" << msg << "\033[0m"
