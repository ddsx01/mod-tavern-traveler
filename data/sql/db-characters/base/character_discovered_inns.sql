CREATE TABLE IF NOT EXISTS `character_discovered_inns` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'Player GUID',
    `zone_id` INT UNSIGNED NOT NULL COMMENT 'AreaTable ID',
    `map_id` INT UNSIGNED NOT NULL,
    `pos_x` FLOAT NOT NULL,
    `pos_y` FLOAT NOT NULL,
    `pos_z` FLOAT NOT NULL,
    PRIMARY KEY (`guid`, `zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Player historical hearthstone binds';
