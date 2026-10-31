#pragma once

class ProcessHelper
{
  public:
    static void RunAsAdmin(const std::wstring &filePath);
    static void OpenFileLocation(const std::wstring &filePath);
    static void UninstallUwpByAumid(const std::wstring &aumid, std::function<void()> onComplete = nullptr);
};
