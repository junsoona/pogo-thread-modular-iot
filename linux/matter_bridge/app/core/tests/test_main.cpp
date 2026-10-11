#include <cstdio>

void RunValueMappingTests(int *failures);
void RunEndpointStoreTests(int *failures);
void RunBridgeManagerTests(int *failures);
void RunRemovalConvergenceTests(int *failures);
void RunPowerBaseRemovalTests(int *failures);

int main()
{
    int failures = 0;
    RunValueMappingTests(&failures);
    RunEndpointStoreTests(&failures);
    RunBridgeManagerTests(&failures);
    RunRemovalConvergenceTests(&failures);
    RunPowerBaseRemovalTests(&failures);
    std::printf("\n%d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
