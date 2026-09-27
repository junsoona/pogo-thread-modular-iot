#include <cstdio>

void RunSensorKindTests(int *failures);
void RunJsonSnapshotTests(int *failures);
void RunInterfaceAddressTests(int *failures);
void RunHttpServerTests(int *failures);
void RunMultiPowerBaseTests(int *failures);
void RunHubIdentityTests(int *failures);
void RunMdnsAdvertiserTests(int *failures);

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
    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
