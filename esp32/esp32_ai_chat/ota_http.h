#pragma once
#include <Arduino.h>

// WiFi 推送升级：设备开一个极简 HTTP 服务，网页直接把 firmware.bin POST 上来。
//   OPTIONS /ota   预检（CORS）
//   GET     /info  返回版本号
//   POST    /ota   body 为固件二进制，Content-Length 必须是文件大小

#define OTA_HTTP_PORT 8080

void otaHttpBegin();          // 启动后台任务
void otaHttpPoll();           // 主循环调用：把当前 IP 同步给蓝牙"信息"特征
bool otaHttpWriting();        // 正在接收
String otaHttpUrl();          // http://<ip>:8080/ota，未联网时为空

// 统一 API 分发：网页 HTTP 与蓝牙控制台共用的 /api/* 处理。
//   method: "GET"/"POST"；path 含查询串（如 /api/logs?clear=1）
//   pin:    X-Pin 的值；body: POST 的表单原文
//   viaBle: 蓝牙控制台通道（/api/logs 不受「网页日志」开关限制）
// 返回 false = 未知路径；否则 code/out/isText 为响应
bool otaApiDispatch(const String &method, const String &path, const String &pin,
                    const String &body, bool viaBle, int &code, String &out, bool &isText);
