#pragma once

#if defined(BO2_GAME_CAMPAIGN)
#define BO2_GAME_ADDR(zombies, campaign) (campaign)
#else
#define BO2_GAME_ADDR(zombies, campaign) (zombies)
#endif
