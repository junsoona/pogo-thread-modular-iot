#include <cstdio>

void RunSensorKindTests(int *failures);
void RunJsonSnapshotTests(int *failures);
void RunInterfaceAddressTests(int *failures);
void RunHttpServerTests(int *failures);
void RunMultiPowerBaseTests(int *failures);
void RunHubIdentityTests(int *failures);
void RunMdnsAdvertiserTests(int *failures);
void RunJsonValueTests(int *failures);
void RunHomeSettingsTests(int *failures);
void RunSettingsStoreTests(int *failures);
void RunDeviceAuthTests(int *failures);
void RunSettingsApiTests(int *failures);
void RunOtbrHelperTests(int *failures);
void RunEnrollmentTests(int *failures);
void RunEnrollmentApiTests(int *failures);
void RunPowerBaseManagementTests(int *failures);
void RunRemovalApiTests(int *failures);
void RunBuildInfoTests(int *failures);

int main()
{
    int failures = 0;
    RunSensorKindTests(&failures);
    RunJsonSnapshotTests(&failures);
    RunInterfaceAddressTests(&failures);
    RunHttpServerTests(&failures);
    RunMultiPowerBaseTests(&failures);
    RunHubIdentityTests(&failures);
    RunMdnsAdvertiserTests(&failures);
    RunJsonValueTests(&failures);
    RunHomeSettingsTests(&failures);
    RunSettingsStoreTests(&failures);
    RunDeviceAuthTests(&failures);
    RunSettingsApiTests(&failures);
    RunOtbrHelperTests(&failures);
    RunEnrollmentTests(&failures);
    RunEnrollmentApiTests(&failures);
    RunPowerBaseManagementTests(&failures);
    RunRemovalApiTests(&failures);
    RunBuildInfoTests(&failures);
    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
