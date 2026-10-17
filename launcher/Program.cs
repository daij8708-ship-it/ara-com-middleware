using System.Diagnostics;
using System.Net.Http;
using System.Text;

namespace AraComMonitor;

internal static class Program
{
    private const string DefaultDistro = "Ubuntu-22.04";
    private const int DefaultPort = 8099;

    private static async Task<int> Main(string[] args)
    {
        try
        {
            Console.OutputEncoding = Encoding.UTF8;
        }
        catch
        {
            // 某些重定向终端不支持切换编码，不影响启动。
        }

        var root = FindRepoRoot();
        if (root is null)
        {
            Console.Error.WriteLine("找不到项目根目录：需要 scripts/monitor.sh 与 CMakeLists.txt。");
            Pause();
            return 2;
        }

        var script = Path.Combine(root, "scripts", "monitor.sh");
        if (!File.Exists(script))
        {
            Console.Error.WriteLine($"找不到脚本：{script}");
            Pause();
            return 2;
        }

        var distro = Environment.GetEnvironmentVariable("ARA_COM_WSL_DISTRO");
        if (string.IsNullOrWhiteSpace(distro)) distro = DefaultDistro;

        var port = ResolvePort(args);
        var noOpen = args.Any(arg => string.Equals(arg, "--no-open", StringComparison.Ordinal));
        var forwarded = args.ToList();
        if (!noOpen) forwarded.Add("--no-open");

        Console.WriteLine("ara-com 运行监测启动器");
        Console.WriteLine($"项目目录: {root}");
        Console.WriteLine($"WSL 发行版: {distro}");
        Console.WriteLine($"面板端口: {port}");
        Console.WriteLine("页面内可切换 DDS / SOME/IP、启停演示、按类型运行测试。");
        Console.WriteLine();

        var startInfo = new ProcessStartInfo
        {
            FileName = "wsl.exe",
            UseShellExecute = false,
            WorkingDirectory = root
        };
        startInfo.ArgumentList.Add("-d");
        startInfo.ArgumentList.Add(distro);
        startInfo.ArgumentList.Add("--cd");
        startInfo.ArgumentList.Add(root);
        startInfo.ArgumentList.Add("--");
        startInfo.ArgumentList.Add("bash");
        startInfo.ArgumentList.Add("scripts/monitor.sh");
        foreach (var arg in forwarded) startInfo.ArgumentList.Add(arg);

        using var child = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
        var exit = new TaskCompletionSource<int>(TaskCreationOptions.RunContinuationsAsynchronously);
        child.Exited += (_, _) =>
        {
            try { exit.TrySetResult(child.ExitCode); }
            catch { exit.TrySetResult(1); }
        };

        try
        {
            child.Start();
        }
        catch (Exception error)
        {
            Console.Error.WriteLine($"无法启动 WSL：{error.Message}");
            Console.Error.WriteLine("请确认已安装 WSL，且发行版名称正确。");
            Pause();
            return 2;
        }

        ConsoleCancelEventHandler cancelHandler = (_, eventArgs) =>
        {
            eventArgs.Cancel = true;
            KillChild(child);
        };
        Console.CancelKeyPress += cancelHandler;

        var baseUrl = $"http://127.0.0.1:{port}/";
        if (!noOpen)
        {
            _ = Task.Run(async () =>
            {
                if (await WaitForHealthAsync(baseUrl + "api/health", child, TimeSpan.FromSeconds(90)))
                {
                    Console.WriteLine();
                    Console.WriteLine($"监测面板: {baseUrl}");
                    try
                    {
                        Process.Start(new ProcessStartInfo
                        {
                            FileName = baseUrl,
                            UseShellExecute = true
                        });
                    }
                    catch (Exception error)
                    {
                        Console.WriteLine($"无法自动打开浏览器，请手动访问上面的地址：{error.Message}");
                    }
                }
                else if (!child.HasExited)
                {
                    Console.WriteLine();
                    Console.WriteLine($"面板未在 90 秒内就绪，请手动打开 {baseUrl}");
                }
            });
        }
        else
        {
            Console.WriteLine($"已禁用自动打开浏览器；面板地址: {baseUrl}");
        }

        var exitCode = await exit.Task;
        Console.CancelKeyPress -= cancelHandler;
        Console.WriteLine();
        Console.WriteLine($"监测进程结束，退出码 {exitCode}。");
        if (exitCode != 0) Pause();
        return exitCode;
    }

    private static string? FindRepoRoot()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        for (var depth = 0; depth < 6 && directory is not null; ++depth, directory = directory.Parent)
        {
            if (File.Exists(Path.Combine(directory.FullName, "scripts", "monitor.sh")) &&
                File.Exists(Path.Combine(directory.FullName, "CMakeLists.txt")))
            {
                return directory.FullName;
            }
        }
        return null;
    }

    private static int ResolvePort(string[] args)
    {
        // 与 monitor.sh 的优先级保持一致：命令行 --port 覆盖环境变量。
        for (var index = 0; index < args.Length; ++index)
        {
            if (string.Equals(args[index], "--port", StringComparison.Ordinal) &&
                index + 1 < args.Length &&
                int.TryParse(args[index + 1], out var value) &&
                value > 0 && value <= 65535)
            {
                return value;
            }
        }

        var fromEnvironment = Environment.GetEnvironmentVariable("ARA_COM_MONITOR_PORT");
        if (int.TryParse(fromEnvironment, out var environmentPort) &&
            environmentPort > 0 && environmentPort <= 65535)
        {
            return environmentPort;
        }
        return DefaultPort;
    }

    private static async Task<bool> WaitForHealthAsync(string url, Process child, TimeSpan timeout)
    {
        using var client = new HttpClient { Timeout = TimeSpan.FromSeconds(1) };
        var deadline = DateTime.UtcNow + timeout;
        while (DateTime.UtcNow < deadline)
        {
            if (child.HasExited) return false;
            try
            {
                using var response = await client.GetAsync(url);
                if (response.IsSuccessStatusCode) return true;
            }
            catch
            {
                // 面板尚未监听，继续等待。
            }
            await Task.Delay(300);
        }
        return false;
    }

    private static void KillChild(Process child)
    {
        try
        {
            if (!child.HasExited) child.Kill(entireProcessTree: true);
        }
        catch
        {
            // 退出路径尽力而为，脚本自身也有 cleanup trap。
        }
    }

    private static void Pause()
    {
        Console.WriteLine("按 Enter 关闭窗口...");
        try { Console.ReadLine(); } catch { }
    }
}
