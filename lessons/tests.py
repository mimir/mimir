from django.test import TestCase
from django.core.urlresolvers import reverse
from django.test import LiveServerTestCase
from selenium import webdriver
from selenium.webdriver.chrome.webdriver import WebDriver

class SimpleTest(TestCase):
    def test_basic_views(self):
        response = client.get('/')
        response.status_code

        
class MySeleniumTests(LiveServerTestCase):
    fixtures = ['user-data.json']

    @classmethod
    def setUpClass(cls):
        cls.selenium = WebDriver()
        super(MySeleniumTests, cls).setUpClass()

    @classmethod
    def tearDownClass(cls):
        cls.selenium.quit()
        super(MySeleniumTests, cls).tearDownClass()

    def test_login(self):
        self.selenium.get('%s%s' % (self.live_server_url, '/login/'))
        username_input = self.selenium.find_element_by_name("username")
        username_input.send_keys('TestUser')
        password_input = self.selenium.find_element_by_name("password")
        password_input.send_keys('BlaTestUserBla123')
        self.selenium.find_element_by_xpath('//input[@value="login"]').click()
        
    def test_register(self):
        self.selenium.get('%s%s' % (self.live_server_url, '/register/'))
        username_input = self.selenium.find_element_by_name("username")
        username_input.send_keys('TestUser')
        password_1_input = self.selenium.find_element_by_name("password1")
        password_1_input.send_keys('BlaTestUserBla123')
        password_2_input = self.selenium.find_element_by_name("password2")
        password_2_input.send_keys('BlaTestUserBla123')
        self.selenium.find_element_by_xpath('//input[@value="Create the account"]').click()
        
    def test_views(self):
        self.selenium.get('%s%s' % (self.live_server_url, '/register/'))
        self.selenium.get('%s%s' % (self.live_server_url, '/login/'))
        self.selenium.get('%s%s' % (self.live_server_url, '/home/'))
        self.selenium.get('%s%s' % (self.live_server_url, '/lessons/'))
        self.selenium.get('%s%s' % (self.live_server_url, '/profile/'))
        self.selenium.get('%s%s' % (self.live_server_url, '/skills/'))
        self.selenium.get('%s%s' % (self.live_server_url, '/users/'))
        self.selenium.get('%s%s' % (self.live_server_url, '/register/'))
    
    def test_register_login(self):
        self.selenium.get('%s%s' % (self.live_server_url, '/register/'))
        username_input = self.selenium.find_element_by_name("username")
        username_input.send_keys('TestUser')
        password_1_input = self.selenium.find_element_by_name("password1")
        password_1_input.send_keys('BlaTestUserBla123')
        password_2_input = self.selenium.find_element_by_name("password2")
        password_2_input.send_keys('BlaTestUserBla123')
        self.selenium.find_element_by_xpath('//input[@value="Create the account"]').click()
        self.selenium.get('%s%s' % (self.live_server_url, '/logout/'))
        self.selenium.get('%s%s' % (self.live_server_url, '/login/'))
        username_input = self.selenium.find_element_by_name("username")
        username_input.send_keys('TestUser')
        password_input = self.selenium.find_element_by_name("password")
        password_input.send_keys('BlaTestUserBla123')
        self.selenium.find_element_by_xpath('//input[@value="login"]').click()
        