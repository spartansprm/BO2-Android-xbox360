return function(fixture)
  local dvars, commands, object, methods = fixture.dvars, fixture.commands, fixture.object, fixture.methods
  local exec = Engine.ExecNow
  Engine.ExecNow = function(controller, command)
    local scopedController = string.match(command, "^bo2zm_player ([0-3]) ")
    if scopedController then assert(tonumber(scopedController) == controller, "outer/scoped controllers differ") end
    return exec(controller, command)
  end
  local primary = 1
  UIExpression.IsPrimaryLocalClient = function(controller) return controller == primary and 1 or 0 end
  local function rows(menu)
    local result = {}
    for _, row in ipairs(menu.buttonList.children) do if row.label then result[row.label] = row end end
    return result
  end
  local function page(name, controller) return LUI.createMenu["Bo2zmExtras" .. (name == "main" and "" or "_" .. name)](controller) end
  local function pick(menu, label, index, controller)
    local row = assert(rows(menu)[label], "missing selector: " .. label)
    return row.m_choices[index].callback({controller = controller or menu:getOwner()}, true)
  end
  local function press(menu, label, controller)
    local row = assert(rows(menu)[label], "missing action: " .. label)
    local event = {name = row.action or "button_action", controller = controller or menu:getOwner(), button = row}
    return (row.action and menu or row):processEvent(event)
  end
  local function scoped(controller, original)
    for i = #commands, 1, -1 do
      if not string.find(commands[i], "bo2zm_log ", 1, true) then
        assert(commands[i] == "bo2zm_player " .. controller .. " " .. original, commands[i])
        return
      end
    end
    error("no scoped command")
  end
  local function count()
    local result = 0
    for _, command in ipairs(commands) do if not string.find(command, "bo2zm_log ", 1, true) then result = result + 1 end end
    return result
  end
  local function unchanged(fn)
    local before = count()
    fn()
    assert(count() == before, "refused event sent a command")
  end
  local function seed(controller, option, value) dvars["bo2zm_p" .. controller .. "_" .. option .. "_ui"] = value end

  dvars.bo2zm_cheats_ui, dvars.bo2zm_god_ui, dvars.bo2zm_hud_ui = 1, 1, 1
  seed(0, "cheats", 0); seed(0, "god", 0); seed(0, "hud", 1)
  seed(1, "cheats", 1); seed(1, "god", 1); seed(1, "hud", 1)
  local zero, one = page("main", 0), page("main", 1)
  assert(zero:getOwner() == 0 and one:getOwner() == 1)
  assert(not zero.anyControllerAllowed and not one.anyControllerAllowed)
  assert(rows(zero)["GOD MODE"].m_dvarName == "bo2zm_p0_god_ui")
  assert(rows(one)["GOD MODE"].m_dvarName == "bo2zm_p1_god_ui")
  assert(rows(zero)["GOD MODE"]:getCurrentValue() == "0" and rows(one)["GOD MODE"]:getCurrentValue() == "1")
  assert(not rows(zero)["GAME SPEED"] and rows(one)["GAME SPEED"])
  assert(not rows(zero)["DOORS + POWER"] and rows(one)["DOORS + POWER"])
  unchanged(function() assert(pick(zero, "GOD MODE", 2) == false) end)
  unchanged(function() pick(one, "GOD MODE", 2, 0) end)
  pick(one, "GOD MODE", 1); scoped(1, "bo2zm_god 0")
  pick(zero, "CHEATS", 2); scoped(0, "bo2zm_cheats 1")
  seed(0, "cheats", 1)
  pick(zero, "GOD MODE", 2); scoped(0, "bo2zm_god 1")
  seed(0, "god", 1); seed(1, "god", 0)
  zero:processEvent({name = "bo2zm_refresh_selectors"})
  one:processEvent({name = "bo2zm_refresh_selectors"})
  assert(rows(zero)["GOD MODE"]:getCurrentValue() == "1" and rows(one)["GOD MODE"]:getCurrentValue() == "0")
  press(zero, "GIVE MAX POINTS"); scoped(0, "bo2zm_max_points")
  unchanged(function() press(zero, "GIVE MAX POINTS", 1) end)

  unchanged(function() one:processEvent({name = "bo2zm_refresh_selectors"}) end)
  seed(1, "cheats", nil)
  unchanged(function() press(one, "GIVE MAX POINTS") end)
  seed(1, "cheats", 1)
  for controller = 2, 3 do
    seed(controller, "cheats", 1)
    pick(page("main", controller), "INFINITE AMMO", 2); scoped(controller, "bo2zm_ammo 1")
  end

  local movement0, movement1 = page("movement", 0), page("movement", 1)
  assert(not rows(movement0)["MOVE SPEED"] and not rows(movement0).GRAVITY and not rows(movement0)["UNLIMITED SPRINT"])
  assert(rows(movement0).NOCLIP and rows(movement0).COLLISION and rows(movement0).COMBO)
  assert(rows(movement1)["MOVE SPEED"].m_dvarName == "bo2zm_hold_g_speed")
  pick(movement1, "MOVE SPEED", 2); scoped(1, "bo2zm_hold g_speed 250")
  pick(movement1, "MOVE SPEED", 1); scoped(1, "bo2zm_release g_speed 190")
  pick(movement0, "NOCLIP", 2); scoped(0, "bo2zm_noclip 1")
  seed(0, "cheats", 0)
  pick(movement0, "COMBO", 2); scoped(0, "bo2zm_combo 1")
  unchanged(function() assert(pick(movement0, "NOCLIP", 2) == false) end)
  seed(0, "cheats", 1)
  local zombies0, zombies1 = page("zombies", 0), page("zombies", 1)
  assert(rows(zombies1).ROUND.m_dvarName == "bo2zm_round_ui")
  assert(rows(zombies1)["STOP SPAWNS"].m_dvarName == "bo2zm_zm_nospawn_ui")
  assert(rows(zombies0)["ZOMBIES IGNORE PLAYERS"])
  assert(not rows(zombies0)["ZOMBIES FREEZE"] and not rows(zombies0)["STOP SPAWNS"]
    and not rows(zombies0)["KILL ALL ZOMBIES"] and not rows(zombies0).ROUND)
  pick(zombies0, "ZOMBIES IGNORE PLAYERS", 2); scoped(0, "bo2zm_zm_ignore 1")
  pick(zombies1, "ROUND", 5); scoped(1, "bo2zm_round 4")
  press(zombies1, "ROUND"); scoped(1, "bo2zm_goto_round")
  press(zombies1, "KILL ALL ZOMBIES"); scoped(1, "bo2zm_kill_zombies")
  local doors0, doors1 = page("doors_power", 0), page("doors_power", 1)
  assert(#doors0.buttonList.children == 0)
  for label, command in pairs({["OPEN ALL DOORS"] = "bo2zm_doors open", ["POWER OFF"] = "bo2zm_power off",
      ["REMOVE FREE DEBRIS"] = "bo2zm_debris remove"}) do
    press(doors1, label); scoped(1, command)
    unchanged(function()
      doors0:processEvent({name = "bo2zm_page_button", controller = 0, button = rows(doors1)[label]})
    end)
  end

  primary = 0
  unchanged(function() pick(movement1, "MOVE SPEED", 2); press(zombies1, "ROUND"); press(doors1, "POWER OFF") end)
  primary = 1
  local identity = UIExpression.IsPrimaryLocalClient
  UIExpression.IsPrimaryLocalClient = nil
  unchanged(function() press(zombies1, "ROUND") end)
  assert(not rows(page("main", 1))["GAME SPEED"])
  UIExpression.IsPrimaryLocalClient = identity

  dvars.bo2zm_map_perks_ui = "specialty_armorvest"
  seed(0, "cross_perks", 0); seed(1, "cross_perks", 1)
  local perks0, perks1 = page("perks", 0), page("perks", 1)
  assert(not rows(perks0)["QUICK REVIVE (OTHER MAP)"] and rows(perks1)["QUICK REVIVE (OTHER MAP)"])
  press(perks0, "JUGGERNOG"); scoped(0, "bo2zm_perk specialty_armorvest")
  press(perks1, "REMOVE PERKS"); scoped(1, "bo2zm_remove_perks")
  pick(perks0, "CROSS-MAP PERKS", 2); scoped(0, "bo2zm_cross_perks 1")
  dvars.bo2zm_map_powerups_ui = "full_ammo"
  press(page("powerups", 0), "MAX AMMO"); scoped(0, "bo2zm_powerup full_ammo")


  function methods:setText(text) self.text = text end
  function methods:addRightButtonPrompt() end
  CoD.HintText.new = function() return object() end
  CoD.ButtonPrompt.new = function() return object() end
  dvars.bo2zm_guns_ui_count, dvars.bo2zm_guns_ui_0 = 1, "mp5_zm,MP5,smg,1"
  seed(0, "give_pap", 0); seed(1, "give_pap", 1)
  local weapons0, weapons1 = page("weapons", 0), page("weapons", 1)
  local function find(element, label)
    if element.label == label then return element end
    for _, child in ipairs(element.children) do local result = find(child, label); if result then return result end end
  end
  assert(find(weapons0, "PACK-A-PUNCH").m_dvarName == "bo2zm_p0_give_pap_ui")
  weapons0:processEvent({name = "bo2zm_toggle_pap", controller = 0}); scoped(0, "bo2zm_give_pap 1")
  weapons1:processEvent({name = "bo2zm_toggle_pap", controller = 1}); scoped(1, "bo2zm_give_pap 0")
  weapons0:processEvent({name = "bo2zm_page_button", controller = 0, button = find(weapons0, "MP5")})
  scoped(0, "bo2zm_give_weapon mp5_zm")
  weapons1:processEvent({name = "bo2zm_page_button", controller = 1, button = find(weapons1, "PACK-A-PUNCH CURRENT WEAPON")})
  scoped(1, "bo2zm_pap_weapon")
  unchanged(function() weapons1:processEvent({name = "bo2zm_toggle_pap", controller = 0}) end)


  local hud0, hud1 = LUI.createMenu.HUD(0), LUI.createMenu.HUD(1)
  LUI.roots = {UIRoot0 = object(), UIRoot2 = object(), UIRootFull = object()}
  LUI.roots.UIRoot2:addElement(hud0); LUI.roots.UIRoot0:addElement(hud1)
  assert(hud0.bo2zmHud and hud1.bo2zmHud and hud0.alpha == 0 and hud1.alpha == 0)
  local camera0, camera1 = page("camera", 0), page("camera", 1)
  assert(rows(camera0).HUD.m_dvarName == "bo2zm_p0_hud_ui" and rows(camera1).HUD.m_dvarName == "bo2zm_p1_hud_ui")
  for _, option in ipairs({{"THIRD PERSON", "third"}, {"TPP CROSSHAIR", "third_crosshair"},
      {"TPP CROSSHAIR TYPE", "tpp_type"}, {"TPP CROSSHAIR COLOR", "tpp_color"}, {"FIELD OF VIEW", "fov"}}) do
    assert(rows(camera0)[option[1]].m_dvarName == "bo2zm_p0_" .. option[2] .. "_ui")
    assert(rows(camera1)[option[1]].m_dvarName == "bo2zm_p1_" .. option[2] .. "_ui")
  end
  pick(camera0, "HUD", 1); scoped(0, "bo2zm_hud 0")
  assert(hud0.alpha == 1 and hud1.alpha == 0)
  if fixture.reviewCase ~= "bot_focus" then

    assert(dvars.bo2zm_p0_hud_ui == 1 and dvars.bo2zm_p1_hud_ui == 1)
    pick(camera1, "HUD", 1); scoped(1, "bo2zm_hud 0")
    assert(hud0.alpha == 1 and hud1.alpha == 1, "controller1 HUD action rehid controller0 before publication")

    pick(camera1, "HUD", 2); scoped(1, "bo2zm_hud 1")
    assert(hud0.alpha == 1 and hud1.alpha == 0, "controller1 HUD restore changed controller0")
  end
  seed(0, "hud", 0)
  hud0:processEvent({name = "bo2zm_hud_check"}); hud1:processEvent({name = "bo2zm_hud_check"})
  assert(hud0.alpha == 1 and hud1.alpha == 0)
  pick(zero, "CHEATS", 1); scoped(0, "bo2zm_cheats 0")
  assert(rows(zero)["GOD MODE"]:getCurrentValue() == "1", "master off discarded the wanted pick")
  assert(hud1.alpha == 0)
  seed(0, "cheats", 0)

  hud1.children = {}
  hud1:setOwner(0)
  LUI.bo2zm.install(); assert(hud1.alpha == 1)
  hud1:setOwner(1)
  LUI.bo2zm.install(); assert(hud1.alpha == 0)

  local late = CoD.Menu.NewFromState("HUD", {})
  LUI.roots.UIRootFull:addElement(late)
  assert(late.alpha ~= 0)
  late:setOwner(1)
  late:processEvent({name = "bo2zm_hud_check"}); assert(late.alpha == 0)
  late:setOwner(0)
  late:processEvent({name = "bo2zm_hud_check"}); assert(late.alpha == 1)
  seed(1, "cheats", 0)
  LUI.bo2zm.install()
  assert(hud1.alpha == 1 and hud0.alpha == 1)
  seed(1, "cheats", 1)
  LUI.bo2zm.install()
  assert(hud1.alpha == 0 and hud0.alpha == 1)

  local before = count()
  press(zero, "RESET CHEATS"); scoped(0, "bo2zm_reset_cheats")
  assert(count() == before + 1 and hud0.alpha == 1 and hud1.alpha == 0)
  before = count()
  local first = #commands + 1
  press(one, "RESET CHEATS")
  assert(count() == before + 7 and hud1.alpha == 1)
  for i = first, #commands do
    if not string.find(commands[i], "bo2zm_log ", 1, true) then
      assert(string.sub(commands[i], 1, 15) == "bo2zm_player 1 ")
      assert(not string.find(commands[i], ";", 1, true), "scoped reset contains an unscoped tail")
    end
  end
  scoped(1, "bo2zm_reset_cheats")
  assert(not LUI.bo2zm.execPlayer(nil, "bo2zm_god 1") and not LUI.bo2zm.execPlayer(-1, "bo2zm_god 1"))

  seed(1, "cheats", 0)
  local experimental, debug = page("experimental", 1), page("debug", 1)
  assert(rows(experimental)["CPU CORES"].m_dvarName == "bo2zm_cpu_cores_ui")
  pick(experimental, "CPU CORES", 1); assert(commands[#commands] == "bo2zm_cpu_cores 0")
  assert(rows(debug).DEBUG.m_dvarName == "bo2zm_dbg_ui")
  pick(debug, "DEBUG", 2); assert(commands[#commands] == "bo2zm_dbg 1")
  local graphics = LUI.createMenu.GraphicsSettings(1)
  assert(graphics.anaglyphButton.m_dvarName == "bo2zm_preset_ui")
  graphics.anaglyphButton.m_choices[1].callback({}, true)
  assert(commands[#commands] == "bo2zm_preset 0")

  CoD.isZombie = false
  unchanged(function() press(one, "GIVE MAX POINTS"); pick(one, "GOD MODE", 2) end)
  assert(not LUI.bo2zm.execPlayer(1, "bo2zm_god 1"))
  hud1:processEvent({name = "bo2zm_hud_check"}); assert(hud1.alpha == 1)
  CoD.isZombie, CoD.isSinglePlayer = true, true
  unchanged(function() press(one, "GIVE MAX POINTS"); pick(one, "GOD MODE", 2) end)
  assert(not LUI.bo2zm.execPlayer(1, "bo2zm_god 1"))
  CoD.isSinglePlayer = false
  Engine.ExecNow = exec
  print("player cheat UI: independent pages/masters, scoped actions, leader1 world gates, owner HUDs and shared controls passed")
end
