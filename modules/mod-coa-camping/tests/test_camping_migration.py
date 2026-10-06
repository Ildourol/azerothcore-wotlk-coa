import argparse
import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
MIGRATION = ROOT / 'data/sql/updates/pending_db_world/rev_20261005_01_coa_camping.sql'
spec = importlib.util.spec_from_file_location(
    'camping_mysql_fixture', ROOT / 'apps/test-framework/test_enchantment_migrations.py')
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class CampingMigration(unittest.TestCase):
    mysql_bin = None
    query = staticmethod(fixture.EnchantmentMigrations.query)

    @classmethod
    def setUpClass(cls):
        fixture.EnchantmentMigrations.mysql_bin = cls.mysql_bin
        cls.addClassCleanup(fixture.EnchantmentMigrations.doClassCleanups)
        fixture.EnchantmentMigrations.setUpClass()
        schema = (ROOT / 'data/sql/base/db_world/gameobject_template.sql').read_text(encoding='utf-8')
        cls.query(schema.split('-- Dumping data for table', 1)[0])

    def setUp(self):
        self.query('TRUNCATE TABLE `gameobject_template`;')
        self.query("INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`) "
                   "VALUES (29784, 8, 192, 'Basic Campfire');")

    def apply(self):
        self.query(MIGRATION.read_text(encoding='utf-8'))

    def rows(self):
        return self.query('SELECT `entry`, `type`, `displayId`, `name`, `size` '
                          'FROM `gameobject_template` ORDER BY `entry`;')

    def test_installation_and_repeat_preserve_native_fire(self):
        self.apply()
        expected = [('29784', '8', '192', 'Basic Campfire', '1'),
                    ('9500200', '10', '345', 'Campsite Supplies', '1'),
                    ('9500201', '5', '100', 'Incense Candle', '1')]
        self.assertEqual(self.rows(), expected)
        self.apply()
        self.assertEqual(self.rows(), expected)

    def test_colliding_entries_are_never_overwritten(self):
        self.query("INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `size`) "
                   "VALUES (9500200, 3, 42, 'Other module', 2), (9500201, 5, 999, 'Incense Candle', 4);")
        before = self.rows()
        self.apply()
        self.assertEqual(self.rows(), before)
        self.apply()
        self.assertEqual(self.rows(), before)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--mysql-bin', required=True, type=Path)
    options, remaining = parser.parse_known_args()
    CampingMigration.mysql_bin = options.mysql_bin.resolve()
    unittest.main(argv=[__file__, *remaining])
