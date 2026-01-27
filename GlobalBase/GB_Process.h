#ifndef GLOBALBASE_PROCESS_H_H
#define GLOBALBASE_PROCESS_H_H

#include "GlobalBasePort.h"

// 判断当前进程是否“以管理员方式运行”
GLOBALBASE_PORT bool GB_IsRunningAsAdmin();

/**
 * @brief 确保当前进程以管理员/root 权限运行；若不是则请求提权并重新启动自身。
 *
 * 行为：
 * - 若已是管理员/root：返回 true。
 * - 若不是：
 *   - Windows：使用 ShellExecuteEx + "runas" 触发 UAC 提权启动新进程；成功后退出当前进程。
 *   - Linux：使用 execvp 执行 sudo 来启动自身（通常会要求输入密码）；成功后当前进程映像被替换（不返回）。
 *
 * @return true 表示当前进程已处于管理员/root 权限；false 表示提权/重启失败（或被用户取消）。
 */
GLOBALBASE_PORT bool GB_EnsureRunningAsAdmin();


























#endif