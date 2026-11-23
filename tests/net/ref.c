/* The provisioning request as Microchip's own 19.5.2 header lays it out,
 * filled the way WiFi101's startProvision does (with our names). */
#include <string.h>
#include "driver/include/m2m_types.h"
void ref_prov(unsigned char out[204])
{
    tstrM2MProvisionModeConfig c;
    memset(&c, 0, sizeof c);
    strcpy((char *)c.strApConfig.au8SSID, "megaminer-setup");
    c.strApConfig.u8ListenChannel = 1;
    c.strApConfig.u8SecType = M2M_WIFI_SEC_OPEN;
    c.strApConfig.u8SsidHide = SSID_MODE_VISIBLE;
    c.strApConfig.au8DHCPServerIP[0] = 192; c.strApConfig.au8DHCPServerIP[1] = 168;
    c.strApConfig.au8DHCPServerIP[2] = 1;   c.strApConfig.au8DHCPServerIP[3] = 1;
    strcpy(c.acHttpServerDomainName, "megaminer.setup");
    c.u8EnableRedirect = 1;
    memcpy(out, &c, 204);
}
