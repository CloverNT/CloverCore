const OS_NAMES: Record<string, string> = {win32: "Windows", linux: "Linux", darwin: "macOS"};

export function platformName(platform: string): string {
    return OS_NAMES[platform] ?? platform;
}
