/* Compiled as C, on purpose: include/ewr/ewr_c.h is what another language
 * links against, and C++-only syntax creeping into it would break every such
 * caller while the C++ suite still passed. Registered as its own CTest case,
 * so all three CI platforms compile and run it.
 *
 * It touches no hardware and opens no session: this is a compile-and-link
 * check with a couple of pure calls to keep the linker honest. */

#include <ewr/ewr_c.h>

#include <stdio.h>
#include <string.h>

int main(void)
{
    ewr_event event;
    const char* name;

    if (ewr_abi_version() < 1)
    {
        printf("[FAIL] ewr_abi_version() < 1\n");
        return 1;
    }

    if (ewr_json_contract_version() < 1)
    {
        printf("[FAIL] ewr_json_contract_version() < 1\n");
        return 1;
    }

    if (ewr_version() == NULL || ewr_version()[0] == '\0')
    {
        printf("[FAIL] ewr_version() is empty\n");
        return 1;
    }

    name = ewr_status_name(EWR_ERR_BLOCKED);
    if (name == NULL || strcmp(name, "blocked") != 0)
    {
        printf("[FAIL] ewr_status_name(EWR_ERR_BLOCKED) = %s\n", name ? name : "(null)");
        return 1;
    }

    /* An unknown code still answers, which is what lets a caller built against
     * a newer header log something instead of crashing. */
    if (strcmp(ewr_status_name(31337), "unknown") != 0)
    {
        printf("[FAIL] an unknown status has no name\n");
        return 1;
    }

    /* The struct a caller declares must be the struct EWR fills. */
    memset(&event, 0, sizeof(event));
    event.size = sizeof(event);
    if (event.size < sizeof(size_t) + 2 * sizeof(const char*))
    {
        printf("[FAIL] ewr_event is smaller than its own fields\n");
        return 1;
    }

    /* Freeing nothing is documented as safe; a caller's cleanup path does it. */
    ewr_string_free(NULL);

    printf("[OK] C ABI header compiles as C and links (EWR %s, ABI %d, JSON v%d)\n",
           ewr_version(), ewr_abi_version(), ewr_json_contract_version());
    return 0;
}
