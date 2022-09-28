--[[
    Module cray-zmqnet
    Copyright 2020-2022 Hewlett Packard Enterprise Development LP.
    UNPUBLISHED PROPRIETARY INFORMATION.
    ALL RIGHTS RESERVED.
]]--

-- local vars: define & assign --

-- template variables ----------------------------------------------------------
local INSTALL_ROOT       = "[@%PREFIX_PATH%@]"
local MOD_LEVEL          = "[@%MODULE_VERSION%@]"
--------------------------------------------------------------------------------

local NICKNAME  = "zmqnet"
local PE_DIR    = INSTALL_ROOT .. "/" .. NICKNAME .. "/" .. MOD_LEVEL

 -- module release info variables
local REL_FILE            = PE_DIR .. "/release_info"
local rel_info            = ""
if isFile(REL_FILE) then
    local f = io.open(REL_FILE, "r")
    local data = f:read("*all")
    f:close()
    if data ~= nil then rel_info = data end
end

 -- standard Lmod functions --

help ([[
The modulefile defines the system paths and
variables for the product cray-zmqnet.
]] .. rel_info .. "\n" .. [[
===================================================================
To re-display ]] .. tostring(myModuleName()) .. "/" .. MOD_LEVEL .. [[ release information,
type:    less ]] .. REL_FILE .. "\n" .. [[
===================================================================
]])

whatis("Loads Cray zmqnet.")

 -- environment modifications --

setenv (           "ZMQNET_VERSION",           MOD_LEVEL                     )
setenv (           "CRAY_ZMQNET_VERSION",      MOD_LEVEL                     )
setenv (           "LIBZMQNET_VERSION",        MOD_LEVEL                     )
setenv (           "CRAY_LIBZMQNET_VERSION",   MOD_LEVEL                     )
setenv (           "ZMQNET_INSTALL_DIR",       PE_DIR                        )
setenv (           "CRAY_ZMQNET_PREFIX",       PE_DIR                        )
setenv (           "LIBZMQNET_INSTALL_DIR",    PE_DIR                        )
setenv (           "CRAY_LIBZMQNET_PREFIX",    PE_DIR                        )

setenv (           "PE_ZMQNET_MODULE_NAME",       myModuleName()                )

append_path   (    "PE_PRODUCT_LIST",          "CRAY-ZMQNET"                    )

prepend_path  (    "PE_PKGCONFIG_PRODUCTS",    "PE_ZMQNET"                      )
prepend_path  (    "PKG_CONFIG_PATH",          PE_DIR .. "/lib/pkgconfig"    )
prepend_path  (    "PE_PKG_CONFIG_PATH",       PE_DIR .. "/lib/pkgconfig"    )

prepend_path  (    "CRAY_LD_LIBRARY_PATH",     PE_DIR .. "/lib"              )
prepend_path  (    "LD_LIBRARY_PATH",          PE_DIR .. "/lib"              )
